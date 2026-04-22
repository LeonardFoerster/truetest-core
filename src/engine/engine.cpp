#include "engine.h"
#include "checkpoint.h"
#include "data/data_handler.h"
#include "data/date_parse.h"
#include "execution/portfolio.h"
#include "execution/latency_model.h"
#include "providers/provider.h"
#include "providers/provider_convert.h"
#include "ui/console_dashboard.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string_view>
#include <vector>
#include <queue>
#include <chrono>
#include <iomanip>

engine::engine(std::shared_ptr<data_handler> dh,
               std::shared_ptr<orderbook> ob,
               std::shared_ptr<IStrategy> strategy,
               engine_config config)
    : config_(std::move(config)), data_handler_(std::move(dh)), strategy_(std::move(strategy)),
      portfolio_(config_.initial_balance),
      analytics_(config_.initial_balance, config_.rolling_window, config_.risk_free_rate,
                 config_.periods_per_year, config_.max_equity_points),
      risk_manager_(config_.risk),
      market_maker_(config_.seed != 0 ? MarketMaker(static_cast<unsigned>(config_.seed + 1))
                                      : MarketMaker())
{
    if (ob)
        orderbook_registry_ = OrderbookRegistry();


    if (config_.mode == engine_mode::shadow)
        shadow_tracker_ = std::make_unique<ShadowTracker>();

#ifdef HAS_SQLITE
    if (!config_.db_path.empty())
        store_ = std::make_unique<SqliteStore>(config_.db_path);
#endif

    restore_from_checkpoint();

    if (config_.mode == engine_mode::live)
    {
        auto reconciler = config_.reconciler;
        if (!reconciler && config_.provider)
            reconciler = config_.provider->get_reconciler();
        if (!reconciler)
            reconciler = std::make_shared<NoopReconciler>();

        auto err = reconciler->reconcile(portfolio_, config_.reconcile_tolerance_bps);
        if (!err.empty())
            throw std::runtime_error("reconciliation refused startup: " + err);
    }
}

void engine::write_checkpoint_if_due(std::size_t event_count)
{
    if (config_.checkpoint_path.empty()) return;
    if (config_.checkpoint_interval_events == 0) return;
    if (event_count == 0 || event_count % config_.checkpoint_interval_events != 0) return;

    try {
        auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        checkpoint::write_file(config_.checkpoint_path, portfolio_,
                               static_cast<uint64_t>(event_count), wall_ms);
    } catch (const std::exception& e) {
        std::cerr << "[checkpoint] write failed: " << e.what() << std::endl;
    }
}

void engine::restore_from_checkpoint()
{
    if (config_.resume_checkpoint_path.empty()) return;

    try {
        auto cp = checkpoint::read_file(config_.resume_checkpoint_path);
        std::unordered_map<std::string, position> pos_map;
        pos_map.reserve(cp.positions.size());
        for (const auto& e : cp.positions)
        {
            position p;
            p.qty = e.qty;
            p.cost_basis = e.cost_basis;
            pos_map.emplace(e.symbol, p);
        }
        portfolio_.restore_state(cp.cash, static_cast<std::size_t>(cp.total_trades),
                                 std::move(pos_map));
        std::cerr << "[checkpoint] resumed from " << config_.resume_checkpoint_path
                  << " at event " << cp.event_count << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[checkpoint] restore failed: " << e.what() << std::endl;
    }
}

#ifdef HAS_SQLITE
void engine::record_run_begin()
{
    if (!store_) return;

    char buf[1024];
    const char* mode_str =
        (config_.mode == engine_mode::backtest) ? "backtest" :
        (config_.mode == engine_mode::shadow)   ? "shadow"   : "live";
    std::snprintf(buf, sizeof(buf),
        R"({"mode":"%s","seed":%llu,"initial_balance":%.4f,"threading":"%s","rolling_window":%zu})",
        mode_str,
        static_cast<unsigned long long>(config_.seed),
        config_.initial_balance,
        preset_to_string(config_.threading).c_str(),
        config_.rolling_window);

    try {
        current_run_id_ = store_->begin_run(buf);
    } catch (const std::exception& e) {
        std::cerr << "[runs] begin_run failed: " << e.what() << std::endl;
        current_run_id_.clear();
    }
}

void engine::record_run_end()
{
    if (!store_ || current_run_id_.empty()) return;

    try {
        const auto& a = get_analytics();
        auto report = a.snapshot();
        report.final_equity = portfolio_.get_equity(last_mid_price_);
        store_->end_run(current_run_id_,
                        report.final_equity,
                        report.sharpe_ratio,
                        report.max_drawdown,
                        static_cast<int>(portfolio_.get_total_trades()));
    } catch (const std::exception& e) {
        std::cerr << "[runs] end_run failed: " << e.what() << std::endl;
    }
    current_run_id_.clear();
}
#endif

void engine::set_strategy(std::shared_ptr<IStrategy> strategy)
{
    if (!strategy) return;
    strategy_ = std::move(strategy);

    for (const auto& [symbol, pos] : portfolio_.get_positions()) {
        strategy_->set_position_open(symbol, std::abs(pos.qty) > 1e-12);
    }
}

void engine::switch_symbol(const std::string& new_symbol)
{
#ifdef HAS_WEB_UI
    if (tick_aggregator_) {
        tick_aggregator_->flush();
        tick_aggregator_ = std::make_unique<BarAggregator>(
            tick_bar_interval_,
            [this](const market_event& bar) {
                broadcast_market_with_indicators(bar);
            });
    }
    bar_history_.clear();
#endif

    data_handler_->db_data_symbol.clear();
    data_handler_->db_data_symbol.push_back(new_symbol);

    strategy_->set_position_open(new_symbol, false);

#ifdef HAS_WEB_UI
    if (ws_worker_) {
        ws_worker_->broadcast(R"({"type":"chart_reset","data":{}})");
        ws_worker_->broadcast_status("running",
            config_.provider ? config_.provider->name() : "",
            new_symbol);
    }
#endif
}

std::shared_ptr<IExecutionAdapter> engine::get_adapter(const std::string& symbol)
{
    auto it = execution_adapters_.find(symbol);
    if (it != execution_adapters_.end())
        return it->second;

    std::shared_ptr<IExecutionAdapter> adapter;
    if (config_.mode != engine_mode::shadow &&
        config_.provider && config_.provider->has_execution())
    {
        adapter = config_.provider->get_execution_adapter();
    }
    if (!adapter)
    {
        auto ob = orderbook_registry_.get_or_create(symbol);
        auto local = std::make_shared<LocalBookAdapter>(
            ob, config_.fee_model, config_.fill_model,
            config_.seed != 0 ? static_cast<unsigned>(config_.seed + 2) : config_.fill_rng_seed,
            config_.market_aggression, config_.qty_scale);
        if (config_.debug_fills)
            local->set_debug_fills(true, config_.debug_fills_budget);
        adapter = local;
    }

    execution_adapters_[symbol] = adapter;
    return adapter;
}

void engine::log_event(const event& ev)
{
    if (config_.is_threaded())
        return;
    if (event_logger_)
        event_logger_->log(ev);
}

void engine::publish_event(const event_pointer& ev)
{
    // Fast path: inline-mode backtest with no WS ring has nothing to fan
    // out, and this runs multiple times per bar. Short-circuit before we
    // construct the push lambda or touch any ring state.
    if (config_.threading == thread_preset::inline_mode
#ifdef HAS_WEB_UI
        && !ws_ring_
#endif
       ) {
        return;
    }

    // Push `ev` onto `ring`, or account a drop. `safety` flags rings that
    // feed the halt flag / shadow portfolio — dropping from those in
    // live/shadow leaves the engine trading past a risk limit, so the
    // halt_on_drop policy escalates such a drop into an engine halt.
    auto push = [&](const std::shared_ptr<EventRing>& ring,
                    std::size_t& drops,
                    [[maybe_unused]] const char* name,
                    bool safety
#ifdef HAS_DEBUG
                  , debug::ring_diagnostics& diag
#endif
                   )
    {
        if (!ring) return;
        if (!ring->try_push(ev))
        {
            ++drops;
#ifdef HAS_DEBUG
            diag.on_drop();
#endif
            if (auto* dash = config_.dashboard.get())
            {
                auto& st = dash->stats();
                // Route the drop counter to the matching atomic so the
                // dashboard's "Rings" row reflects the live state. Uses
                // the ring name as a cheap switch — these are string
                // literals so the compare is fast and identity-stable.
                if      (std::string_view(name) == "logging")    st.ring_drops_logging.store(drops, std::memory_order_relaxed);
                else if (std::string_view(name) == "risk")       st.ring_drops_risk.store(drops, std::memory_order_relaxed);
                else if (std::string_view(name) == "stats")      st.ring_drops_stats.store(drops, std::memory_order_relaxed);
                else if (std::string_view(name) == "observer")   st.ring_drops_observer.store(drops, std::memory_order_relaxed);
                else if (std::string_view(name) == "risk_stats") st.ring_drops_risk_stats.store(drops, std::memory_order_relaxed);
                else if (std::string_view(name) == "mm")         st.ring_drops_mm.store(drops, std::memory_order_relaxed);
            }
            if (safety &&
                config_.drop_policy == ring_drop_policy::halt_on_drop &&
                !halt_flag_.exchange(true, std::memory_order_acq_rel))
            {
                if (auto* dash = config_.dashboard.get())
                {
                    dash->stats().halt_flag.store(true, std::memory_order_release);
                    dash->set_state(truetest::ui::connection_state::halted);
                    char msg[128];
                    std::snprintf(msg, sizeof(msg),
                                  "ring drop on '%s' (%zu dropped) — halted",
                                  name, drops);
                    dash->push_event(truetest::ui::event_severity::error, msg);
                }
                else
                {
                    std::cerr << "  ! ring drop on safety ring '" << name
                              << "' (" << drops
                              << " dropped) with halt_on_drop — halting engine\n";
                }
            }
        }
#ifdef HAS_DEBUG
        else
            diag.on_push(ring->occupancy());
#endif
    };

#ifdef HAS_DEBUG
#  define TT_PUSH(ring, counter, name, safety, diag)   push(ring, counter, name, safety, diag)
#else
#  define TT_PUSH(ring, counter, name, safety, diag)   push(ring, counter, name, safety)
#endif

    switch (config_.threading)
    {
    case thread_preset::inline_mode:
        break;

    case thread_preset::light:
        TT_PUSH(observer_ring_, observer_drops_, "observer", true, observer_diag_);
        break;

    case thread_preset::standard:
        TT_PUSH(logging_ring_,    logging_drops_,    "logging",    false, logging_diag_);
        TT_PUSH(risk_stats_ring_, risk_stats_drops_, "risk_stats", true,  risk_stats_diag_);
        break;

    case thread_preset::full:
        TT_PUSH(logging_ring_, logging_drops_, "logging", false, logging_diag_);
        TT_PUSH(risk_ring_,    risk_drops_,    "risk",    true,  risk_diag_);
        TT_PUSH(stats_ring_,   stats_drops_,   "stats",   false, stats_diag_);
        break;

    case thread_preset::extended:
        TT_PUSH(logging_ring_, logging_drops_, "logging", false, logging_diag_);
        TT_PUSH(risk_ring_,    risk_drops_,    "risk",    true,  risk_diag_);
        TT_PUSH(stats_ring_,   stats_drops_,   "stats",   false, stats_diag_);
        TT_PUSH(mm_ring_,      mm_drops_,      "mm",      false, mm_diag_);
        break;
    }

#undef TT_PUSH

#ifdef HAS_WEB_UI
    if (ws_ring_ && !ws_ring_->try_push(ev))
    {
        ws_drops_++;
        if (ws_drops_ == 1 || ws_drops_ % 1000 == 0)
            std::cerr << "  WS ring: " << ws_drops_ << " events dropped\n";
    }
#endif
}

std::unique_ptr<LoggingWorker> engine::make_logging_worker()
{
    auto text_sink = LoggingWorker::log_sink::none;
    if (config_.log_to_stdout)
        text_sink = LoggingWorker::log_sink::stdout_sink;
    else if (!config_.text_log_path.empty())
        text_sink = LoggingWorker::log_sink::file_sink;

    return std::make_unique<LoggingWorker>(
        config_.event_log_path, text_sink, config_.text_log_path, config_.compress_log,
        config_.log_max_bytes, config_.log_max_files);
}

void engine::pin_event_loop_thread()
{
    if (!config_.is_threaded() || config_.disable_pinning)
        return;

    int core_id = config_.pin_event_loop;
    if (core_id < 0)
    {
        auto core_map = build_core_map();
        for (const auto& ca : core_map)
            if (ca.role == core_role::event_loop) { core_id = ca.core_id; break; }
    }

    pin_current_thread(core_id);
}

void engine::start_workers()
{
    halt_flag_.store(false, std::memory_order_release);
    worker_failed_.store(false, std::memory_order_release);

    auto wire_failure = [this](Worker& w) {
        w.set_failure_flag(worker_failed_);
        w.set_spin_policy(config_.worker_spin_policy);
        w.set_max_consecutive_errors(config_.max_consecutive_worker_errors);
    };

#ifdef HAS_WEB_UI
    if (config_.enable_web_ui)
    {
        ws_ring_ = std::make_shared<EventRing>();
        ws_worker_ = std::make_unique<WebSocketWorker>(config_.ws_port, config_.ws_compress);
        wire_failure(*ws_worker_);

#ifdef HAS_SQLITE
        if (store_)
        {
            ws_worker_->set_on_list_runs([this](int limit) -> std::string {
                if (!store_) return {};
                try { return store_->query_runs_json(limit); }
                catch (const std::exception& e) {
                    std::cerr << "[runs] query_runs failed: " << e.what() << std::endl;
                    return {};
                }
            });
        }
#endif

        ws_worker_->set_on_metrics([this]() -> std::string {
            auto snap = analytics_.snapshot();
            char buf[2048];
            std::size_t n = 0;
            auto w = [&](const char* name, const char* help, const char* type,
                         double value) {
                n += std::snprintf(buf + n, sizeof(buf) - n,
                    "# HELP %s %s\n# TYPE %s %s\n%s %.6f\n",
                    name, help, name, type, name, value);
            };
            w("truetest_events_processed_total",
              "Events processed by analytics", "counter",
              static_cast<double>(snap.total_orders + snap.total_fills));
            w("truetest_orders_submitted_total",
              "Total orders submitted", "counter",
              static_cast<double>(snap.total_orders));
            w("truetest_fills_total",
              "Total fills executed", "counter",
              static_cast<double>(snap.total_fills));
            w("truetest_equity_current",
              "Current portfolio equity (valued at last_mid_price)", "gauge",
              portfolio_.get_equity(last_mid_price_));
            w("truetest_cash_current",
              "Current cash balance", "gauge",
              portfolio_.get_cash());
            w("truetest_drawdown_current",
              "Current drawdown fraction", "gauge",
              snap.max_drawdown);
            w("truetest_sharpe_ratio",
              "Sharpe ratio (point-in-time)", "gauge",
              snap.sharpe_ratio);
            w("truetest_halt_flag",
              "1 if halt flag set, 0 otherwise", "gauge",
              halt_flag_.load(std::memory_order_relaxed) ? 1.0 : 0.0);
            return std::string(buf, n);
        });

        worker_threads_.emplace_back([this]() {
            ws_worker_->run(*ws_ring_);
        });
    }
#endif

    if (!config_.is_threaded())
        return;

    auto core_map = build_core_map();

    auto find_core = [&](core_role role) -> int {
        if (config_.disable_pinning) return -1;
        switch (role) {
        case core_role::event_loop: if (config_.pin_event_loop >= 0) return config_.pin_event_loop; break;
        case core_role::logging:    if (config_.pin_logging >= 0)    return config_.pin_logging; break;
        case core_role::risk:       if (config_.pin_risk >= 0)       return config_.pin_risk; break;
        case core_role::stats:      if (config_.pin_stats >= 0)      return config_.pin_stats; break;
        case core_role::market_maker: if (config_.pin_mm >= 0)       return config_.pin_mm; break;
        }
        for (const auto& ca : core_map)
            if (ca.role == role) return ca.core_id;
        return -1;
    };

    switch (config_.threading)
    {
    case thread_preset::inline_mode:
        return;

    case thread_preset::light:
    {
        observer_ring_ = std::make_shared<EventRing>();
        observer_worker_ = std::make_unique<ObserverWorker>(risk_manager_, halt_flag_,
            config_.initial_balance, config_.rolling_window, config_.risk_free_rate,
            config_.periods_per_year, config_.max_equity_points);
        wire_failure(*observer_worker_);

        worker_threads_.emplace_back([this]() {
            observer_worker_->run(*observer_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::logging));
        break;
    }

    case thread_preset::standard:
    {
        logging_ring_ = std::make_shared<EventRing>();
        risk_stats_ring_ = std::make_shared<EventRing>();

        logging_worker_ = make_logging_worker();
        risk_stats_worker_ = std::make_unique<RiskStatsWorker>(risk_manager_, halt_flag_,
            config_.initial_balance, config_.rolling_window, config_.risk_free_rate,
            config_.periods_per_year, config_.max_equity_points);
        wire_failure(*logging_worker_);
        wire_failure(*risk_stats_worker_);

        worker_threads_.emplace_back([this]() {
            logging_worker_->run(*logging_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::logging));

        worker_threads_.emplace_back([this]() {
            risk_stats_worker_->run(*risk_stats_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::risk));
        break;
    }

    case thread_preset::full:
    {
        logging_ring_ = std::make_shared<EventRing>();
        risk_ring_ = std::make_shared<EventRing>();
        stats_ring_ = std::make_shared<EventRing>();

        logging_worker_ = make_logging_worker();
        risk_worker_ = std::make_unique<RiskWorker>(risk_manager_, halt_flag_,
            config_.initial_balance, config_.rolling_window, config_.risk_free_rate,
            config_.periods_per_year, config_.max_equity_points);
        stats_worker_ = std::make_unique<StatsWorker>(config_.initial_balance, 1000,
            config_.rolling_window, config_.risk_free_rate,
            config_.periods_per_year, config_.max_equity_points);
        wire_failure(*logging_worker_);
        wire_failure(*risk_worker_);
        wire_failure(*stats_worker_);

        worker_threads_.emplace_back([this]() {
            logging_worker_->run(*logging_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::logging));

        worker_threads_.emplace_back([this]() {
            risk_worker_->run(*risk_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::risk));

        worker_threads_.emplace_back([this]() {
            stats_worker_->run(*stats_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::stats));
        break;
    }

    case thread_preset::extended:
    {
        logging_ring_ = std::make_shared<EventRing>();
        risk_ring_ = std::make_shared<EventRing>();
        stats_ring_ = std::make_shared<EventRing>();
        mm_ring_ = std::make_shared<EventRing>();
        mm_order_ring_ = std::make_shared<MMRing>();

        logging_worker_ = make_logging_worker();
        risk_worker_ = std::make_unique<RiskWorker>(risk_manager_, halt_flag_,
            config_.initial_balance, config_.rolling_window, config_.risk_free_rate,
            config_.periods_per_year, config_.max_equity_points);
        stats_worker_ = std::make_unique<StatsWorker>(config_.initial_balance, 1000,
            config_.rolling_window, config_.risk_free_rate,
            config_.periods_per_year, config_.max_equity_points);
        mm_worker_ = std::make_unique<MarketMakerWorker>(
            config_.seed != 0 ? static_cast<unsigned>(config_.seed + 3) : 42u,
            *mm_order_ring_);
        wire_failure(*logging_worker_);
        wire_failure(*risk_worker_);
        wire_failure(*stats_worker_);
        wire_failure(*mm_worker_);

        worker_threads_.emplace_back([this]() {
            logging_worker_->run(*logging_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::logging));

        worker_threads_.emplace_back([this]() {
            risk_worker_->run(*risk_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::risk));

        worker_threads_.emplace_back([this]() {
            stats_worker_->run(*stats_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::stats));

        worker_threads_.emplace_back([this]() {
            mm_worker_->run(*mm_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::market_maker));
        break;
    }
    }
}

void engine::stop_workers()
{
    if (observer_worker_) observer_worker_->stop();
    if (logging_worker_) logging_worker_->stop();
    if (risk_worker_) risk_worker_->stop();
    if (stats_worker_) stats_worker_->stop();
    if (risk_stats_worker_) risk_stats_worker_->stop();
    if (mm_worker_) mm_worker_->stop();
#ifdef HAS_WEB_UI
    if (ws_worker_) ws_worker_->stop();
#endif

    for (auto& t : worker_threads_)
    {
        if (t.joinable())
            t.join();
    }
    worker_threads_.clear();

    if (mm_order_ring_)
    {
        event_pointer ev;
        while (mm_order_ring_->try_pop(ev)) {}
    }

    if (config_.mode == engine_mode::live)
    {
        auto kill_switch = config_.kill_switch;
        if (!kill_switch && config_.provider)
            kill_switch = config_.provider->get_kill_switch();
        if (!kill_switch)
            kill_switch = std::make_shared<NoopKillSwitch>();

        bool ok = kill_switch->cancel_all_and_flatten(config_.kill_switch_deadline);
        if (!ok)
        {
            std::cerr << "  WARNING: kill-switch did NOT complete within "
                      << config_.kill_switch_deadline.count()
                      << " ms — inspect exchange state manually.\n";
        }
    }

    Worker* all_workers[] = {
        logging_worker_.get(), risk_worker_.get(), stats_worker_.get(),
        observer_worker_.get(), risk_stats_worker_.get(), mm_worker_.get(),
#ifdef HAS_WEB_UI
        ws_worker_.get(),
#else
        nullptr,
#endif
    };
    for (auto* w : all_workers)
    {
        if (w && w->has_failed())
        {
            try { std::rethrow_exception(w->get_exception()); }
            catch (const std::exception& e) {
                std::cerr << "  Worker failed: " << e.what() << "\n";
            }
            catch (...) {
                std::cerr << "  Worker failed with unknown exception\n";
            }
        }
    }

    std::size_t ws_d = 0;
#ifdef HAS_WEB_UI
    ws_d = ws_drops_;
#endif
    std::size_t total_drops = logging_drops_ + risk_drops_ + stats_drops_
                            + observer_drops_ + risk_stats_drops_ + mm_drops_ + ws_d;
    if (total_drops > 0)
    {
        std::cerr << "  WARNING: " << total_drops << " events dropped from ring buffers.\n";
        std::cerr << "  Drops: logging=" << logging_drops_
                  << " risk=" << risk_drops_
                  << " stats=" << stats_drops_
                  << " observer=" << observer_drops_
                  << " risk_stats=" << risk_stats_drops_
                  << " mm=" << mm_drops_ << "\n";
    }

    auto report_hwm = [](const char* name, const std::shared_ptr<EventRing>& ring) {
        if (!ring) return;
        auto hwm = ring->high_watermark();
        if (hwm > 0)
        {
            double pct = hwm * 100.0 / ring->capacity();
            std::cerr << "  Ring HWM: " << name << "=" << hwm
                      << "/" << ring->capacity() << " (" << static_cast<int>(pct) << "%)\n";
        }
    };
    report_hwm("logging", logging_ring_);
    report_hwm("risk", risk_ring_);
    report_hwm("stats", stats_ring_);
    report_hwm("observer", observer_ring_);
    report_hwm("risk_stats", risk_stats_ring_);
    report_hwm("mm", mm_ring_);
#ifdef HAS_WEB_UI
    report_hwm("ws", ws_ring_);
#endif
}

#ifdef HAS_WEB_UI
void engine::process_ws_commands(bool& halt_requested, std::size_t& event_count)
{
    if (!ws_worker_) return;

    ws_command cmd;
    while (ws_worker_->poll_command(cmd))
    {
        if (cmd.command == "stop")
        {
            halt_requested = true;
            ws_worker_->broadcast_status("halted",
                config_.provider ? config_.provider->name() : "",
                "");
        }
        else if (cmd.command == "order")
        {
            if (cmd.side.empty() || cmd.quantity <= 0.0)
            {
                ws_worker_->broadcast(event_json::order_response_to_json(
                    0, "rejected", "invalid order parameters"));
                continue;
            }

            auto otype = (cmd.order_type == "limit") ? order_type::limit : order_type::market;
            auto oside = (cmd.side == "sell") ? order_side::sell : order_side::buy;
            double price = (otype == order_type::limit && cmd.price > 0.0) ? cmd.price : last_mid_price_;

            std::string symbol;
            if (!data_handler_->db_data_symbol.empty())
                symbol = data_handler_->db_data_symbol.back();

            auto ts = std::chrono::system_clock::now();
            auto tif = (otype == order_type::limit) ? time_in_force::gtc : time_in_force::ioc;

            // Routed via route_order so WS-submitted orders pick up the same
            // instrument-spec quantization, execution_bar_delay, and latency
            // model as strategy-emitted orders. Bypassing this was a
            // look-ahead leak: manual UI orders previously filled same-bar
            // at last_mid_price_ with no venue filters.
            order_event o(ts, symbol, otype, oside, cmd.quantity, price, tif);
            bool halted_here = false;
            route_order(o, ts, event_count, halted_here);

            // route_order assigns the id and, on rejection, already
            // broadcasts a rejection_event via publish_event + ws_worker_.
            // The explicit "accepted" ack below just acknowledges receipt
            // to the submitting client regardless of downstream outcome.
            ws_worker_->broadcast(event_json::order_response_to_json(
                o.get_order_id(), "accepted", "",
                symbol, cmd.side, cmd.quantity, price));

            if (halted_here) halt_requested = true;
        }
        else if (cmd.command == "set_timeframe" && !cmd.timeframe.empty())
        {
            int value = 0;
            char unit = 0;
            if (std::sscanf(cmd.timeframe.c_str(), "%d%c", &value, &unit) == 2 && value > 0)
            {
                std::chrono::milliseconds new_interval{0};
                switch (unit)
                {
                case 's': new_interval = std::chrono::seconds(value); break;
                case 'm': new_interval = std::chrono::minutes(value); break;
                case 'h': new_interval = std::chrono::hours(value); break;
                default: break;
                }

                if (new_interval.count() > 0)
                {
                    tick_bar_interval_ = new_interval;

                    if (tick_aggregator_)
                    {
                        tick_aggregator_->flush();
                        tick_aggregator_ = std::make_unique<BarAggregator>(
                            tick_bar_interval_,
                            [this](const market_event& bar) {
                                broadcast_market_with_indicators(bar);
                            });
                    }

                    ws_worker_->broadcast(R"({"type":"chart_reset","data":{"timeframe":")"
                        + cmd.timeframe + R"("}})");
                }
            }
        }
#ifdef HAS_SQLITE
        else if (cmd.command == "query_fills")
        {
            int limit = cmd.price > 0 ? static_cast<int>(cmd.price) : 200;
            if (store_)
            {
                ws_worker_->broadcast(event_json::fills_history_to_json(
                    store_->query_fills_json(cmd.timeframe, limit)));
            }
        }
#endif
        else if (cmd.command == "set_symbol" && !cmd.value.empty())
        {
            std::string new_symbol = cmd.value;
            std::transform(new_symbol.begin(), new_symbol.end(),
                           new_symbol.begin(), ::tolower);

            std::lock_guard<std::mutex> lk(switch_mu_);
            pending_symbol_ = new_symbol;

            ws_worker_->broadcast_status("switching",
                config_.provider ? config_.provider->name() : "", new_symbol);
        }
        else if (cmd.command == "set_strategy" && !cmd.value.empty())
        {
            std::string new_strategy = cmd.value;

            auto available = StrategyFactory::available();
            bool valid = std::find(available.begin(), available.end(), new_strategy)
                         != available.end();
            if (!valid) {
                ws_worker_->broadcast(event_json::error_to_json(
                    "Unknown strategy: " + new_strategy));
                continue;
            }

            std::lock_guard<std::mutex> lk(switch_mu_);
            pending_strategy_ = new_strategy;

            ws_worker_->broadcast_status("running",
                new_strategy,
                (!data_handler_->db_data_symbol.empty())
                    ? data_handler_->db_data_symbol.back() : "");
        }
        else if (cmd.command == "backfill")
        {
            (void)cmd;
        }
    }
}

void engine::broadcast_orderbook_snapshot(const std::string& symbol)
{
    if (!ws_worker_) return;

    auto now = std::chrono::steady_clock::now();
    if (now - last_ob_snapshot_time_ < std::chrono::milliseconds(250))
        return;
    last_ob_snapshot_time_ = now;

    auto ob = orderbook_registry_.get(symbol);
    if (!ob) return;

    auto infos = ob->get_order_infos();
    const auto& bid_lvls = infos.get_bids();
    const auto& ask_lvls = infos.get_asks();

    const int max_levels = 20;
    std::vector<std::pair<double, double>> bids;
    std::vector<std::pair<double, double>> asks;

    for (int i = 0; i < std::min(max_levels, static_cast<int>(bid_lvls.size())); ++i)
    {
        bids.emplace_back(bid_lvls[i].price_.to_double(),
                          static_cast<double>(bid_lvls[i].quantity_) / 1e8);
    }
    for (int i = 0; i < std::min(max_levels, static_cast<int>(ask_lvls.size())); ++i)
    {
        asks.emplace_back(ask_lvls[i].price_.to_double(),
                          static_cast<double>(ask_lvls[i].quantity_) / 1e8);
    }

    double spread = 0.0;
    if (!bids.empty() && !asks.empty())
        spread = asks.front().first - bids.front().first;

    ws_worker_->broadcast_orderbook(bids, asks, spread);
}

void engine::send_state_snapshot()
{
    if (!ws_worker_) return;

    ws_worker_->broadcast_status("running",
        config_.provider ? config_.provider->name() : "",
        (!data_handler_->db_data_symbol.empty()) ? data_handler_->db_data_symbol.back() : "");

    {
        auto json = event_json::portfolio_to_json(portfolio_);
        ws_worker_->broadcast(json);
    }

    {
        auto report = analytics_.snapshot();
        report.final_equity = portfolio_.get_equity(last_mid_price_);
        auto json = event_json::analytics_to_json(report);
        ws_worker_->broadcast(json);
    }

    ws_worker_->broadcast(R"({"type":"chart_reset","data":{}})");
    for (const auto& bar_json : bar_history_)
        ws_worker_->broadcast(bar_json);

#ifdef HAS_SQLITE
    if (store_)
    {
        ws_worker_->broadcast(
            event_json::fills_history_to_json(store_->query_fills_json("", 200)));
        ws_worker_->broadcast(
            event_json::equity_history_to_json(store_->query_equity_json(500)));
    }
#endif

    {
        auto names = StrategyFactory::available();
        std::string json = R"({"type":"strategies","data":[)";
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i > 0) json += ",";
            json += "\"" + names[i] + "\"";
        }
        json += "]}";
        ws_worker_->broadcast(json);
    }
}

void engine::broadcast_market_with_indicators(const market_event& mkt)
{
    if (!ws_worker_) return;

    auto indicators = strategy_ ? strategy_->get_indicator_values(mkt.get_symbol())
                                : std::vector<std::pair<std::string, double>>{};

    auto json = event_json::to_json_with_indicators(mkt, indicators);
    ws_worker_->broadcast(json);

    if (bar_history_.size() >= MAX_BAR_HISTORY)
        bar_history_.erase(bar_history_.begin());
    bar_history_.push_back(json);
}
#endif

void engine::print_summary()
{
    switch (config_.threading)
    {
    case thread_preset::inline_mode:
        analytics_.print_report();
        return;
    case thread_preset::light:
        if (observer_worker_)
            observer_worker_->analytics().print_report();
        return;
    case thread_preset::standard:
        if (risk_stats_worker_)
            risk_stats_worker_->analytics().print_report();
        return;
    case thread_preset::full:
    case thread_preset::extended:
        if (stats_worker_)
            stats_worker_->analytics().print_report();
        break;
    }

    if (shadow_tracker_)
        shadow_tracker_->print_report();
}

const Analytics& engine::get_analytics() const
{
    switch (config_.threading)
    {
    case thread_preset::light:
        if (observer_worker_) return observer_worker_->analytics();
        break;
    case thread_preset::standard:
        if (risk_stats_worker_) return risk_stats_worker_->analytics();
        break;
    case thread_preset::full:
    case thread_preset::extended:
        if (stats_worker_) return stats_worker_->analytics();
        break;
    default:
        break;
    }
    return analytics_;
}

bool engine::process_order(const std::shared_ptr<order_event>& o,
                           std::size_t& event_count,
                           bool& halt_requested)
{
    {
        auto snap = analytics_.risk_view();
        auto action = risk_manager_.check_order(*o, portfolio_, snap);
        if (action == risk_action::halt || action == risk_action::reject)
        {
            const char* reason = (action == risk_action::halt)
                ? "risk limit breached - engine halted"
                : "order rejected by risk manager";

            auto rej = std::make_shared<rejection_event>(
                o->get_timestamp(), o->get_symbol(), o->get_order_id(), reason);
            log_event(*rej);
            publish_event(rej);

#ifdef HAS_WEB_UI
            if (ws_worker_)
            {
                ws_worker_->broadcast(event_json::order_response_to_json(
                    o->get_order_id(), "rejected", reason,
                    o->get_symbol(),
                    o->get_side() == order_side::buy ? "buy" : "sell",
                    o->get_quantity(), o->get_price()));
            }
#endif
            order_tracker_.set_status(o->get_order_id(), order_status::rejected);
            if (action == risk_action::halt)
            {
                if (config_.risk_unwind)
                    unwind_positions(event_count);
                halt_requested = true;
                return false;
            }
            return true;
        }
    }

    order_tracker_.set_status(o->get_order_id(), order_status::open);
    log_event(*o);
    publish_event(o);
    analytics_.on_event(o);

    auto adapter = get_adapter(o->get_symbol());

    if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
        local->set_mid_price(last_mid_price_);

    adapter->submit_order(*o);

    if (config_.mode == engine_mode::shadow && config_.provider)
    {
        auto exchange_adapter = config_.provider->get_execution_adapter();
        if (exchange_adapter)
            exchange_adapter->submit_order(*o);
    }

    std::vector<fill_event> fills;
    if (adapter->poll_fills(fills))
    {
        for (auto& f : fills)
        {
            order_tracker_.set_status(f.get_order_id(),
                f.is_partial() ? order_status::partially_filled : order_status::filled);
            auto fill_ptr = fill_pool_.acquire(f);
            log_event(f);
            portfolio_.on_fill(f);
            exit_manager_.on_fill(f);
            risk_manager_.on_fill(f);
#ifdef HAS_SQLITE
            if (store_) store_->insert_fill(f);
#endif
            notify_position_change_all(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
            publish_event(fill_ptr);
            analytics_.on_event(fill_ptr);

            if (config_.mode == engine_mode::shadow && shadow_tracker_)
                shadow_tracker_->on_simulated_fill(f);

#ifdef HAS_WEB_UI
            if (ws_worker_)
            {
                ws_worker_->broadcast(event_json::order_response_to_json(
                    f.get_order_id(), "filled", "",
                    f.get_symbol(),
                    f.get_side() == order_side::buy ? "buy" : "sell",
                    f.get_filled_quantity(), f.get_fill_price()));
            }
#endif

            event_count++;

            {
                auto post_snap = analytics_.risk_view();
                auto post_action = risk_manager_.check_post_fill(f, portfolio_, post_snap);
                if (post_action == risk_action::halt)
                {
#ifdef HAS_WEB_UI
                    if (ws_worker_)
                        ws_worker_->broadcast(event_json::error_to_json(
                            "Risk manager halted engine", "risk"));
#endif
                    if (config_.risk_unwind)
                        unwind_positions(event_count);
                    halt_requested = true;
                    return false;
                }
            }
        }
    }

    if (config_.mode == engine_mode::shadow && config_.provider)
    {
        auto exchange_adapter = config_.provider->get_execution_adapter();
        if (exchange_adapter)
        {
            std::vector<fill_event> exchange_fills;
            if (exchange_adapter->poll_fills(exchange_fills))
            {
                for (auto& ef : exchange_fills)
                {
                    if (shadow_tracker_)
                        shadow_tracker_->on_exchange_fill(ef);
                }
            }
        }
    }

    event_count++;
    return true;
}

bool engine::cancel_order(const std::string& symbol, uint64_t order_id,
                          const std::string& reason)
{
    auto adapter = get_adapter(symbol);
    bool cancelled = adapter->cancel_order(order_id);

    if (!cancelled)
    {
        auto it = std::remove_if(pending_stops_.begin(), pending_stops_.end(),
            [order_id](const std::shared_ptr<order_event>& o) {
                return o->get_order_id() == order_id;
            });
        if (it != pending_stops_.end())
        {
            pending_stops_.erase(it, pending_stops_.end());
            cancelled = true;
        }
    }

    if (cancelled)
    {
        order_tracker_.set_status(order_id, order_status::cancelled);
        auto cancel_ev = std::make_shared<cancel_event>(
            std::chrono::system_clock::now(), symbol, order_id, reason);
        log_event(*cancel_ev);
        publish_event(cancel_ev);
        if (!config_.is_threaded())
            analytics_.on_event(cancel_ev);
    }

    return cancelled;
}

bool engine::modify_order(const std::string& symbol, uint64_t order_id,
                          double new_price, double new_qty)
{
    auto adapter = get_adapter(symbol);
    bool modified = adapter->modify_order(order_id, new_price, new_qty);

    if (modified)
    {
        auto amend_ev = std::make_shared<amend_event>(
            std::chrono::system_clock::now(), symbol, order_id, new_price, new_qty);
        log_event(*amend_ev);
        publish_event(amend_ev);
        if (!config_.is_threaded())
            analytics_.on_event(amend_ev);
    }

    return modified;
}

void engine::unwind_positions(std::size_t& event_count)
{
    // Snapshot (symbol, qty) before iterating — each fill mutates
    // portfolio_.positions_, which would invalidate iterators taken over
    // the live map.
    std::vector<std::pair<std::string, double>> to_close;
    to_close.reserve(portfolio_.get_positions().size());
    for (const auto& [symbol, pos] : portfolio_.get_positions())
    {
        if (std::abs(pos.qty) >= 1e-12)
            to_close.emplace_back(symbol, pos.qty);
    }

    for (const auto& [symbol, qty] : to_close)
    {
        // Sign-aware flatten: long → market SELL qty, short → market BUY -qty.
        // Hardcoding a sell here left shorts open on halt, which is the
        // exact scenario where you most want positions closed.
        const order_side close_side = (qty > 0.0)
            ? order_side::sell : order_side::buy;
        const double close_qty = std::abs(qty);

        auto now = std::chrono::system_clock::now();
        auto close_order = order_pool_.acquire(order_event(
            now, symbol, order_type::market, close_side,
            close_qty, last_mid_price_));
        close_order->set_order_id(OrderIdGenerator::next());
        close_order->set_strategy_name("risk_unwind");

        order_tracker_.set_status(close_order->get_order_id(), order_status::open);
        log_event(*close_order);
        publish_event(close_order);
        analytics_.on_event(close_order);

        auto adapter = get_adapter(symbol);
        if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
            local->set_mid_price(last_mid_price_);

        adapter->submit_order(*close_order);

        std::vector<fill_event> fills;
        if (adapter->poll_fills(fills))
        {
            for (auto& f : fills)
            {
                order_tracker_.set_status(f.get_order_id(),
                    f.is_partial() ? order_status::partially_filled : order_status::filled);
                auto fill_ptr = fill_pool_.acquire(f);
                log_event(f);
                portfolio_.on_fill(f);
                exit_manager_.on_fill(f);
                risk_manager_.on_fill(f);
#ifdef HAS_SQLITE
                if (store_) store_->insert_fill(f);
#endif
                notify_position_change_all(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                publish_event(fill_ptr);
                analytics_.on_event(fill_ptr);

#ifdef HAS_WEB_UI
                if (ws_worker_)
                {
                    ws_worker_->broadcast(event_json::order_response_to_json(
                        f.get_order_id(), "filled", "risk unwind",
                        f.get_symbol(),
                        f.get_side() == order_side::buy ? "buy" : "sell",
                        f.get_filled_quantity(), f.get_fill_price()));
                }
#endif
                event_count++;
            }
        }
    }
}

void engine::apply_l2_snapshot(const std::string& symbol,
                               const std::vector<l2_level>& bids,
                               const std::vector<l2_level>& asks)
{
    // Tag this symbol as L2-driven so MarketMaker::replenish stops
    // seeding paper liquidity on top of it.
    l2_seeded_symbols_.insert(symbol);
    auto ob = orderbook_registry_.get_or_create(symbol);

    std::vector<std::pair<Price, quantity>> ob_bids;
    ob_bids.reserve(bids.size());
    for (const auto& lvl : bids)
        ob_bids.emplace_back(Price::from_double(lvl.price),
                             static_cast<quantity>(lvl.quantity));

    std::vector<std::pair<Price, quantity>> ob_asks;
    ob_asks.reserve(asks.size());
    for (const auto& lvl : asks)
        ob_asks.emplace_back(Price::from_double(lvl.price),
                             static_cast<quantity>(lvl.quantity));

    ob->apply_l2_snapshot(ob_bids, ob_asks);

    auto ev = std::make_shared<l2_snapshot_event>(
        std::chrono::system_clock::now(), symbol, bids, asks);
    log_event(*ev);
    publish_event(ev);
}

void engine::apply_l2_update(const std::string& symbol,
                             tick_side ts_side, double price, int64_t new_qty)
{
    auto ob = orderbook_registry_.get_or_create(symbol);

    side ob_side = (ts_side == tick_side::bid) ? side::buy : side::sell;
    ob->apply_l2_update(ob_side, Price::from_double(price),
                        static_cast<quantity>(new_qty));

    auto ev = std::make_shared<l2_update_event>(
        std::chrono::system_clock::now(), symbol, ts_side, price, new_qty);
    log_event(*ev);
    publish_event(ev);
}

const instrument_spec* engine::resolve_instrument_spec(const std::string& symbol)
{
    auto it = instrument_cache_.find(symbol);
    if (it != instrument_cache_.end())
        return it->second ? &*it->second : nullptr;

    std::optional<instrument_spec> spec;
    auto ov = config_.instrument_overrides.find(symbol);
    if (ov != config_.instrument_overrides.end())
        spec = ov->second;
    else if (config_.provider)
        spec = config_.provider->get_instrument(symbol);

    auto [cit, _] = instrument_cache_.emplace(symbol, std::move(spec));
    return cit->second ? &*cit->second : nullptr;
}

bool engine::apply_instrument_spec(order_event& o, const instrument_spec& spec) const
{
    if (spec.tick_size > 0.0)
    {
        if (o.get_order_type() == order_type::limit ||
            o.get_order_type() == order_type::stop_limit)
        {
            o.set_price(quantize_price_to_tick(o.get_price(), spec.tick_size));
        }
        if (o.get_order_type() == order_type::stop ||
            o.get_order_type() == order_type::stop_limit)
        {
            o.set_stop_price(quantize_price_to_tick(o.get_stop_price(), spec.tick_size));
        }
    }

    if (spec.lot_size > 0.0)
        o.set_quantity(floor_qty_to_lot(o.get_quantity(), spec.lot_size));

    if (!meets_min_qty(o.get_quantity(), spec.min_qty))
        return false;

    const double ref_price = (o.get_order_type() == order_type::stop)
        ? o.get_stop_price()
        : o.get_price();
    if (!meets_min_notional(o.get_quantity(), ref_price, spec.min_notional))
        return false;

    return true;
}

bool engine::route_order(order_event& order,
                         const std::chrono::system_clock::time_point& sim_time,
                         std::size_t& event_count, bool& halt_requested)
{
    order.set_order_id(OrderIdGenerator::next());
    order_tracker_.set_status(order.get_order_id(), order_status::pending);

    if (auto* spec = resolve_instrument_spec(order.get_symbol()))
    {
        if (!apply_instrument_spec(order, *spec))
        {
            const char* reason = "order rejected by venue filter (min_qty/min_notional)";
            auto rej = std::make_shared<rejection_event>(
                order.get_timestamp(), order.get_symbol(),
                order.get_order_id(), reason);
            log_event(*rej);
            publish_event(rej);
#ifdef HAS_WEB_UI
            if (ws_worker_)
            {
                ws_worker_->broadcast(event_json::order_response_to_json(
                    order.get_order_id(), "rejected", reason,
                    order.get_symbol(),
                    order.get_side() == order_side::buy ? "buy" : "sell",
                    order.get_quantity(), order.get_price()));
            }
#endif
            order_tracker_.set_status(order.get_order_id(), order_status::rejected);
            (void)event_count;
            (void)halt_requested;
            return true;
        }
    }

    if (order.get_order_type() == order_type::stop ||
        order.get_order_type() == order_type::stop_limit)
    {
        pending_stops_.push_back(order_pool_.acquire(order));
        return true;
    }

    if (config_.latency_model)
    {
        auto latency = config_.latency_model->get_order_latency();
        order.set_earliest_eligible_ts(sim_time + latency);
        pending_orders_.push({order_pool_.acquire(order), order_seq_++});
        return true;
    }

    if (config_.execution_bar_delay > 0)
    {
        order.set_earliest_eligible_ts(sim_time + std::chrono::nanoseconds(1));
        pending_orders_.push({order_pool_.acquire(order), order_seq_++});
        return true;
    }

    order.set_earliest_eligible_ts(sim_time);
    auto order_ptr = order_pool_.acquire(order);
    if (order.get_tif() == time_in_force::day)
        day_order_ids_.push_back({order.get_symbol(), order.get_order_id()});
    return process_order(order_ptr, event_count, halt_requested);
}

void engine::check_pending_stops(double high, double low,
                                 const std::chrono::system_clock::time_point& sim_time,
                                 std::size_t& event_count, bool& halt_requested)
{
    auto it = pending_stops_.begin();
    while (it != pending_stops_.end() && !halt_requested)
    {
        auto& stop = *it;
        bool triggered = false;

        if (stop->get_side() == order_side::buy && high >= stop->get_stop_price())
            triggered = true;
        else if (stop->get_side() == order_side::sell && low <= stop->get_stop_price())
            triggered = true;

        if (triggered)
        {
            if (stop->get_order_type() == order_type::stop)
            {
                auto market_order = order_pool_.acquire(
                    sim_time, stop->get_symbol(), order_type::market,
                    stop->get_side(), stop->get_quantity(), stop->get_stop_price(),
                    time_in_force::ioc);
                market_order->set_order_id(stop->get_order_id());
                market_order->set_earliest_eligible_ts(sim_time);
                if (market_order->get_tif() == time_in_force::day)
                    day_order_ids_.push_back({market_order->get_symbol(), market_order->get_order_id()});
                process_order(market_order, event_count, halt_requested);
            }
            else
            {
                auto limit_order = order_pool_.acquire(
                    sim_time, stop->get_symbol(), order_type::limit,
                    stop->get_side(), stop->get_quantity(), stop->get_price(),
                    stop->get_tif());
                limit_order->set_order_id(stop->get_order_id());
                limit_order->set_earliest_eligible_ts(sim_time);
                if (limit_order->get_tif() == time_in_force::day)
                    day_order_ids_.push_back({limit_order->get_symbol(), limit_order->get_order_id()});
                process_order(limit_order, event_count, halt_requested);
            }
            it = pending_stops_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void engine::notify_position_change_all(const std::string& symbol, bool open)
{
    if (strategy_) strategy_->set_position_open(symbol, open);
    for (auto& s : additional_strategies_)
        if (s) s->set_position_open(symbol, open);

    // When the position closes via the strategy's own signal (or a
    // manual flatten), drop any armed/pending exit intent for that
    // (strategy, symbol). Otherwise the next entry would still carry
    // the prior intent armed against a stale opener id.
    if (!open)
    {
        if (!primary_strategy_name_.empty())
            exit_manager_.cancel(primary_strategy_name_, symbol);
        for (const auto& name : additional_strategy_names_)
            exit_manager_.cancel(name, symbol);
    }
}

void engine::register_strategy_exit_intent(IStrategy& strategy,
                                           const std::string& strategy_name,
                                           std::uint64_t order_id)
{
    auto intent = strategy.take_pending_exit_intent();
    if (!intent) return;
    intent->opener_order_id = order_id;
    intent->strategy_name   = strategy_name;  // engine-assigned key
    exit_manager_.register_pending(std::move(*intent));
}

bool engine::evaluate_exits(const std::string& symbol, double px,
                            std::chrono::system_clock::time_point ts,
                            std::size_t& event_count,
                            std::int64_t recv_ns)
{
    auto close_opt = exit_manager_.on_price(symbol, px, ts);
    if (!close_opt) return false;
    close_opt->set_recv_ns(recv_ns);
    bool halt = false;
    route_order(*close_opt, ts, event_count, halt);
    return halt;
}

void engine::dispatch_extras_on_market(const market_event& mkt,
                                       const std::chrono::system_clock::time_point& ts,
                                       std::size_t& event_count)
{
    if (additional_strategies_.empty()) return;
    for (std::size_t i = 0; i < additional_strategies_.size(); ++i)
    {
        auto& s = additional_strategies_[i];
        if (!s) continue;

        if (evaluate_exits(mkt.get_symbol(), mkt.get_close(), ts,
                           event_count, mkt.get_recv_ns()))
            return;

        if (auto o = s->on_market(mkt))
        {
            o->set_recv_ns(mkt.get_recv_ns());
            o->set_strategy_name(additional_strategy_names_[i]);
            std::uint64_t oid = o->get_order_id();
            bool halt = false;
            route_order(*o, ts, event_count, halt);
            if (!halt)
                register_strategy_exit_intent(*s, additional_strategy_names_[i], oid);
            if (halt) return;
        }
    }
}

void engine::dispatch_extras_on_tick(const tick_event& te,
                                     const std::chrono::system_clock::time_point& ts,
                                     std::size_t& event_count)
{
    if (additional_strategies_.empty()) return;
    for (std::size_t i = 0; i < additional_strategies_.size(); ++i)
    {
        auto& s = additional_strategies_[i];
        if (!s) continue;

        if (evaluate_exits(te.get_symbol(), te.get_price(), ts,
                           event_count, te.get_recv_ns()))
            return;

        if (auto o = s->on_tick(te))
        {
            o->set_recv_ns(te.get_recv_ns());
            o->set_strategy_name(additional_strategy_names_[i]);
            std::uint64_t oid = o->get_order_id();
            bool halt = false;
            route_order(*o, ts, event_count, halt);
            if (!halt)
                register_strategy_exit_intent(*s, additional_strategy_names_[i], oid);
            if (halt) return;
        }
    }
}

void engine::process_single_bar(const bar_record& rec, std::size_t& event_count,
                                const std::chrono::system_clock::time_point& timestamp)
{
    market_event mkt(
        timestamp,
        rec.symbol,
        rec.open,
        rec.high,
        rec.low,
        rec.close,
        rec.volume
    );
    mkt.set_recv_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    last_mid_price_ = mkt.get_open();

    while (!pending_orders_.empty() &&
           pending_orders_.top().order->get_earliest_eligible_ts() <= timestamp)
    {
        auto entry = pending_orders_.top();
        pending_orders_.pop();
        if (entry.order->get_tif() == time_in_force::day)
            day_order_ids_.push_back({entry.order->get_symbol(), entry.order->get_order_id()});
        bool halt = false;
        if (!process_order(entry.order, event_count, halt)) break;
    }

    last_mid_price_ = mkt.get_close();

    auto ob = orderbook_registry_.get_or_create(mkt.get_symbol());
    if (!preset_has_mm_worker(config_.threading) &&
        !l2_seeded_symbols_.count(mkt.get_symbol()))
        market_maker_.replenish(ob, last_mid_price_);

    if (config_.provider && config_.provider->has_execution())
    {
        config_.provider->on_mid_price(mkt.get_symbol(), last_mid_price_);
        auto provider_adapter = config_.provider->get_execution_adapter();

        // Shadow bar-path: feed a synthetic trade at the bar close with the
        // bar's volume. This is a lossy proxy (we don't know the intra-bar
        // path) but it's strictly more informative than no trade feed, and
        // bar-only shadow is already low-fidelity for fills by construction.
        if (config_.mode == engine_mode::shadow && provider_adapter)
        {
            provider_adapter->on_trade(mkt.get_symbol(), mkt.get_close(),
                                       static_cast<double>(mkt.get_volume()),
                                       mkt.get_timestamp());
        }

        std::vector<fill_event> provider_fills;
        if (provider_adapter && provider_adapter->poll_fills(provider_fills))
        {
            for (auto& f : provider_fills)
            {
                if (config_.mode == engine_mode::shadow)
                {
                    if (shadow_tracker_)
                        shadow_tracker_->on_exchange_fill(f);
                    continue;
                }
                auto fill_ptr = fill_pool_.acquire(f);
                portfolio_.on_fill(f);
                exit_manager_.on_fill(f);
                notify_position_change_all(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                publish_event(fill_ptr);
                if (!config_.is_threaded())
                    analytics_.on_event(fill_ptr);
#ifdef HAS_WEB_UI
                if (ws_worker_) {
                    ws_worker_->broadcast(event_json::order_response_to_json(
                        f.get_order_id(), "filled", "",
                        f.get_symbol(),
                        f.get_side() == order_side::buy ? "buy" : "sell",
                        f.get_filled_quantity(), f.get_fill_price()));
                }
#endif
                event_count++;
            }
        }
    }

    auto mkt_ptr = market_pool_.acquire(mkt);
    auto order_opt = strategy_->on_market(mkt);
    if (order_opt) order_opt->set_recv_ns(mkt.get_recv_ns());
    log_event(mkt);
    publish_event(mkt_ptr);
    if (!config_.is_threaded())
        analytics_.on_event(mkt_ptr);
    event_count++;

#ifdef HAS_SQLITE
    if (store_)
    {
        auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp.time_since_epoch()).count();
        store_->insert_equity_point(ts_ms, portfolio_.get_equity(last_mid_price_));
    }
#endif

#ifdef HAS_WEB_UI
    broadcast_market_with_indicators(mkt);
#endif

    if (evaluate_exits(mkt.get_symbol(), mkt.get_close(), timestamp,
                       event_count, mkt.get_recv_ns()))
        return;

    if (order_opt)
    {
        if (!primary_strategy_name_.empty())
            order_opt->set_strategy_name(primary_strategy_name_);
        std::uint64_t oid = order_opt->get_order_id();
        bool halt = false;
        route_order(*order_opt, timestamp, event_count, halt);
        if (!halt && strategy_)
            register_strategy_exit_intent(*strategy_, primary_strategy_name_, oid);
    }
    dispatch_extras_on_market(mkt, timestamp, event_count);
}

void engine::process_single_tick(const tick_record& rec, std::size_t& event_count)
{
    tick_side ts = tick_side::unknown;
    if (rec.side == data_tick_side::bid) ts = tick_side::bid;
    else if (rec.side == data_tick_side::ask) ts = tick_side::ask;

    tick_event te(rec.timestamp, rec.symbol, rec.price, rec.quantity, ts);
    te.set_recv_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    last_mid_price_ = rec.price;

    auto ob = orderbook_registry_.get_or_create(rec.symbol);
    if (!l2_seeded_symbols_.count(rec.symbol))
        market_maker_.replenish(ob, last_mid_price_);

    if (config_.provider && config_.provider->has_execution())
    {
        config_.provider->on_mid_price(rec.symbol, last_mid_price_);
        auto provider_adapter = config_.provider->get_execution_adapter();

        // Shadow: feed the real trade tape into the provider adapter so it
        // can match open orders against what actually printed. Must fire
        // BEFORE poll_fills so fills generated by this tick are drained in
        // the same iteration.
        if (config_.mode == engine_mode::shadow && provider_adapter)
        {
            provider_adapter->on_trade(rec.symbol, rec.price,
                                       static_cast<double>(rec.quantity),
                                       rec.timestamp);
        }

        std::vector<fill_event> provider_fills;
        if (provider_adapter && provider_adapter->poll_fills(provider_fills))
        {
            for (auto& f : provider_fills)
            {
                if (config_.mode == engine_mode::shadow)
                {
                    if (shadow_tracker_)
                        shadow_tracker_->on_exchange_fill(f);
                    continue;
                }
                auto fill_ptr = fill_pool_.acquire(f);
                portfolio_.on_fill(f);
                exit_manager_.on_fill(f);
                notify_position_change_all(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                publish_event(fill_ptr);
                if (!config_.is_threaded())
                    analytics_.on_event(fill_ptr);
#ifdef HAS_WEB_UI
                if (ws_worker_) {
                    ws_worker_->broadcast(event_json::order_response_to_json(
                        f.get_order_id(), "filled", "",
                        f.get_symbol(),
                        f.get_side() == order_side::buy ? "buy" : "sell",
                        f.get_filled_quantity(), f.get_fill_price()));
                }
#endif
                event_count++;
            }
        }
    }

    bool halt = false;

    while (!pending_orders_.empty() &&
           pending_orders_.top().order->get_earliest_eligible_ts() <= rec.timestamp)
    {
        auto entry = pending_orders_.top();
        pending_orders_.pop();
        if (entry.order->get_tif() == time_in_force::day)
            day_order_ids_.push_back({entry.order->get_symbol(), entry.order->get_order_id()});
        if (!process_order(entry.order, event_count, halt)) break;
    }
    if (halt) return;

    check_pending_stops(rec.price, rec.price, rec.timestamp, event_count, halt);
    if (halt) return;

    auto tick_ptr = tick_pool_.acquire(te);
    log_event(te);
    publish_event(tick_ptr);
    if (!config_.is_threaded())
        analytics_.on_event(tick_ptr);
    event_count++;

#ifdef HAS_SQLITE
    if (store_)
    {
        auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            rec.timestamp.time_since_epoch()).count();
        store_->insert_equity_point(ts_ms, portfolio_.get_equity(last_mid_price_));
    }
#endif

#ifdef HAS_WEB_UI
    if (tick_aggregator_)
        tick_aggregator_->on_tick(rec.symbol, rec.price, rec.quantity, rec.timestamp);
#endif

    if (evaluate_exits(rec.symbol, rec.price, rec.timestamp,
                       event_count, te.get_recv_ns()))
        return;

    auto order_opt = strategy_->on_tick(te);
    if (order_opt)
    {
        order_opt->set_recv_ns(te.get_recv_ns());
        if (!primary_strategy_name_.empty())
            order_opt->set_strategy_name(primary_strategy_name_);
        std::uint64_t oid = order_opt->get_order_id();
        route_order(*order_opt, rec.timestamp, event_count, halt);
        if (!halt && strategy_)
            register_strategy_exit_intent(*strategy_, primary_strategy_name_, oid);
    }
    dispatch_extras_on_tick(te, rec.timestamp, event_count);
}

void engine::run_streaming(std::shared_ptr<DataBridge<bar_record>> bridge)
{
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    if (!config_.event_log_path.empty())
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path, config_.compress_log);

#ifdef HAS_SQLITE
    record_run_begin();
#endif

    start_workers();
    pin_event_loop_thread();

    const auto start = std::chrono::high_resolution_clock::now();
    std::size_t event_count = 0;
    std::size_t bar_index = 0;

    auto* dash = config_.dashboard.get();
    if (dash)
    {
        dash->set_state(truetest::ui::connection_state::waiting);
        dash->push_event(truetest::ui::event_severity::notice,
                         "streaming: waiting for first bar");
    }
    else
    {
        std::cout << "\rStreaming: waiting for data..." << std::flush;
    }

    auto last_report_time = std::chrono::steady_clock::now();

    bridge->run_streaming(data_handler_, [&](const bar_record& rec) {
        auto timestamp = [&]() {
            if (!rec.date.empty()) {
                try {
                    int64_t ts_ms = std::stoll(rec.date);
                    if (ts_ms > 1000000000000LL)
                        return std::chrono::system_clock::time_point(
                            std::chrono::milliseconds(ts_ms));
                } catch (...) {}
            }
            return std::chrono::system_clock::now();
        }();
        process_single_bar(rec, event_count, timestamp);
        bar_index++;

        if (dash)
        {
            auto& st = dash->stats();
            if (bar_index == 1)
                dash->set_state(truetest::ui::connection_state::live);
            st.events_total.store(bar_index, std::memory_order_relaxed);
            st.fills_total.store(portfolio_.get_total_fills(),
                                 std::memory_order_relaxed);
            st.trades_total.store(portfolio_.get_total_trades(),
                                  std::memory_order_relaxed);
            st.last_price_fp8.store(
                static_cast<std::int64_t>(rec.close * 1e8),
                std::memory_order_relaxed);
        }

#ifdef HAS_WEB_UI
        broadcast_orderbook_snapshot(rec.symbol);
        bool halt = false;
        process_ws_commands(halt, event_count);
        if (ws_worker_ && ws_worker_->has_pending_connect())
            send_state_snapshot();
#endif

        {
            std::lock_guard<std::mutex> lk(switch_mu_);
            if (!pending_symbol_.empty()) {
                std::string new_sym = std::move(pending_symbol_);
                pending_symbol_.clear();
                switch_symbol(new_sym);
            }
            if (!pending_strategy_.empty()) {
                std::string new_strat = std::move(pending_strategy_);
                pending_strategy_.clear();
                strategy_params params;
                params.balance = config_.initial_balance;
                set_strategy(StrategyFactory::create(new_strat, params));
            }
        }

        if (!dash)
        {
            auto now_report = std::chrono::steady_clock::now();
            if (now_report - last_report_time >= std::chrono::milliseconds(200))
            {
                std::cout << "\rStreaming: " << bar_index
                          << " bars | Fills: " << portfolio_.get_total_fills()
                          << " | Round-trips: " << portfolio_.get_total_trades()
                          << std::flush;
                last_report_time = now_report;
            }
        }
    });

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    if (dash)
    {
        dash->set_state(truetest::ui::connection_state::closed);
    }
    else
    {
        std::cout << std::endl;
        std::cout << "Streaming complete: " << bar_index << " bars, "
                  << portfolio_.get_total_trades() << " trades in "
                  << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;
    }

#ifdef HAS_SQLITE
    if (store_) store_->flush_all();
    record_run_end();
#endif
    if (event_logger_) event_logger_->flush();
    stop_workers();
}

void engine::run_streaming(std::shared_ptr<DataBridge<tick_record>> bridge)
{
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    if (!config_.event_log_path.empty())
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path, config_.compress_log);

#ifdef HAS_SQLITE
    record_run_begin();
#endif

#ifdef HAS_WEB_UI
    tick_aggregator_ = std::make_unique<BarAggregator>(
        tick_bar_interval_,
        [this](const market_event& bar) {
            broadcast_market_with_indicators(bar);
        });
#endif

    start_workers();
    pin_event_loop_thread();

    const auto start = std::chrono::high_resolution_clock::now();
    std::size_t event_count = 0;
    std::size_t tick_count = 0;

    auto* dash = config_.dashboard.get();
    if (dash)
    {
        dash->set_state(truetest::ui::connection_state::waiting);
        dash->push_event(truetest::ui::event_severity::notice,
                         "streaming: waiting for first tick");
    }
    else
    {
        std::cout << "\rStreaming: waiting for data..." << std::flush;
    }

    auto last_report_time = std::chrono::steady_clock::now();

    bridge->run_streaming(data_handler_, [&](const tick_record& rec) {
        process_single_tick(rec, event_count);
        tick_count++;

        if (dash)
        {
            auto& st = dash->stats();
            if (tick_count == 1)
                dash->set_state(truetest::ui::connection_state::live);
            st.events_total.store(tick_count, std::memory_order_relaxed);
            st.fills_total.store(portfolio_.get_total_fills(),
                                 std::memory_order_relaxed);
            st.trades_total.store(portfolio_.get_total_trades(),
                                  std::memory_order_relaxed);
            st.last_price_fp8.store(
                static_cast<std::int64_t>(rec.price * 1e8),
                std::memory_order_relaxed);
        }

#ifdef HAS_WEB_UI
        broadcast_orderbook_snapshot(rec.symbol);
        bool halt = false;
        process_ws_commands(halt, event_count);
        if (ws_worker_ && ws_worker_->has_pending_connect())
            send_state_snapshot();
#endif

        {
            std::lock_guard<std::mutex> lk(switch_mu_);
            if (!pending_symbol_.empty()) {
                std::string new_sym = std::move(pending_symbol_);
                pending_symbol_.clear();
                switch_symbol(new_sym);
            }
            if (!pending_strategy_.empty()) {
                std::string new_strat = std::move(pending_strategy_);
                pending_strategy_.clear();
                strategy_params params;
                params.balance = config_.initial_balance;
                set_strategy(StrategyFactory::create(new_strat, params));
            }
        }

        if (!dash)
        {
            auto now_report = std::chrono::steady_clock::now();
            if (now_report - last_report_time >= std::chrono::milliseconds(200))
            {
                std::cout << "\rStreaming: " << tick_count
                          << " ticks | Fills: " << portfolio_.get_total_fills()
                          << " | Round-trips: " << portfolio_.get_total_trades()
                          << std::flush;
                last_report_time = now_report;
            }
        }
    });

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    if (dash)
    {
        dash->set_state(truetest::ui::connection_state::closed);
    }
    else
    {
        std::cout << std::endl;
        std::cout << "Streaming complete: " << tick_count << " ticks, "
                  << portfolio_.get_total_trades() << " trades in "
                  << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;
    }

#ifdef HAS_WEB_UI
    if (tick_aggregator_) tick_aggregator_->flush();
#endif

#ifdef HAS_SQLITE
    if (store_) store_->flush_all();
    record_run_end();
#endif
    if (event_logger_) event_logger_->flush();
    stop_workers();
}

void engine::run_streaming(std::shared_ptr<DataBridge<provider::event>> bridge)
{
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    if (!config_.event_log_path.empty())
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path, config_.compress_log);

#ifdef HAS_SQLITE
    record_run_begin();
#endif

    start_workers();
    pin_event_loop_thread();

    const auto start = std::chrono::high_resolution_clock::now();
    std::size_t event_count = 0;
    std::size_t record_count = 0;

    auto* dash = config_.dashboard.get();
    if (dash)
    {
        dash->set_state(truetest::ui::connection_state::waiting);
        dash->push_event(truetest::ui::event_severity::notice,
                         "streaming: waiting for first event");
    }
    else
    {
        std::cout << "\rStreaming: waiting for data..." << std::flush;
    }

    auto last_report_time = std::chrono::steady_clock::now();

    // Unified dispatch: the engine stops caring which venue produced the
    // frame. Each provider::event arm routes into a path that already
    // exists — bar/tick feed the strategy, L2 populates orderbook_registry_
    // directly. This is how the LocalBookAdapter sees real exchange depth
    // in shadow mode without leaking Binance specifics into the engine.
    bridge->run_streaming(data_handler_, [&](const provider::event& ev) {
        std::visit([&](const auto& e) {
            using E = std::decay_t<decltype(e)>;

            if constexpr (std::is_same_v<E, provider::bar>)
            {
                auto rec = provider::to_bar_record(e);
                auto timestamp = [&]() {
                    if (!rec.date.empty()) {
                        try {
                            int64_t ts_ms = std::stoll(rec.date);
                            if (ts_ms > 1000000000000LL)
                                return std::chrono::system_clock::time_point(
                                    std::chrono::milliseconds(ts_ms));
                        } catch (...) {}
                    }
                    return std::chrono::system_clock::now();
                }();
                process_single_bar(rec, event_count, timestamp);
                record_count++;
            }
            else if constexpr (std::is_same_v<E, provider::tick>)
            {
                auto rec = provider::to_tick_record(e);
                process_single_tick(rec, event_count);
                record_count++;
            }
            else if constexpr (std::is_same_v<E, provider::l2_snapshot>)
            {
                std::vector<l2_level> bids;
                bids.reserve(e.bids.size());
                for (const auto& lvl : e.bids)
                    bids.push_back({lvl.price, lvl.quantity});
                std::vector<l2_level> asks;
                asks.reserve(e.asks.size());
                for (const auto& lvl : e.asks)
                    asks.push_back({lvl.price, lvl.quantity});
                apply_l2_snapshot(e.symbol, bids, asks);
            }
            else if constexpr (std::is_same_v<E, provider::l2_update>)
            {
                tick_side ts = (e.side == 0) ? tick_side::bid : tick_side::ask;
                apply_l2_update(e.symbol, ts, e.price, e.new_quantity);
            }
            // provider::status falls through — informational only.
        }, ev);

        if (dash)
        {
            auto& st = dash->stats();
            if (record_count == 1)
                dash->set_state(truetest::ui::connection_state::live);
            st.events_total.store(record_count, std::memory_order_relaxed);
            st.fills_total.store(portfolio_.get_total_fills(),
                                 std::memory_order_relaxed);
            st.trades_total.store(portfolio_.get_total_trades(),
                                  std::memory_order_relaxed);
            if (last_mid_price_ > 0.0)
                st.last_price_fp8.store(
                    static_cast<std::int64_t>(last_mid_price_ * 1e8),
                    std::memory_order_relaxed);
        }

#ifdef HAS_WEB_UI
        bool halt = false;
        process_ws_commands(halt, event_count);
        if (ws_worker_ && ws_worker_->has_pending_connect())
            send_state_snapshot();
#endif

        if (!dash)
        {
            auto now_report = std::chrono::steady_clock::now();
            if (now_report - last_report_time >= std::chrono::milliseconds(200))
            {
                std::cout << "\rStreaming: " << record_count
                          << " events | Fills: " << portfolio_.get_total_fills()
                          << " | Round-trips: " << portfolio_.get_total_trades()
                          << std::flush;
                last_report_time = now_report;
            }
        }
    });

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    if (dash)
    {
        dash->set_state(truetest::ui::connection_state::closed);
    }
    else
    {
        std::cout << std::endl;
        std::cout << "Streaming complete: " << record_count << " events, "
                  << portfolio_.get_total_trades() << " trades in "
                  << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;
    }

#ifdef HAS_SQLITE
    if (store_) store_->flush_all();
    record_run_end();
#endif
    if (event_logger_) event_logger_->flush();
    stop_workers();
}

void engine::run()
{
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    if (data_handler_->has_tick_data())
    {
        run_tick_data();
        return;
    }

    if (!data_handler_->has_bar_data()) {
        throw std::runtime_error("no data loaded — call IDataSource::load_data() before run()");
    }

#ifdef HAS_SQLITE
    record_run_begin();
#endif

    if (!config_.event_log_path.empty())
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path, config_.compress_log);

#ifdef HAS_DEBUG
    memory_sampler_.set_start(debug::memory_snapshot::capture());
#endif

    start_workers();
    pin_event_loop_thread();

#ifdef HAS_WEB_UI
    if (ws_worker_)
    {
        std::string sym = (!data_handler_->db_data_symbol.empty()) ? data_handler_->db_data_symbol[0] : "";
        ws_worker_->broadcast_status("running", "", sym);
    }
#endif

    const auto base_ts = (config_.seed != 0)
        ? std::chrono::system_clock::time_point(std::chrono::milliseconds(0))
        : std::chrono::system_clock::now();
    const auto n = data_handler_->db_data_symbol.size();
    analytics_.reserve_hint(n);
    const auto start = std::chrono::high_resolution_clock::now();
    std::cout << "\rProgress: 0.000% | Trades executed: 0" << std::flush;

    std::size_t event_count = 0;
    auto last_report_time = std::chrono::steady_clock::now();

    pending_stops_.clear();
    while (!pending_orders_.empty()) pending_orders_.pop();
    order_seq_ = 0;
    day_order_ids_.clear();

    bool halt_requested = false;

    // Bar-timestamp resolution. Consumed by analytics (equity curve x-axis),
    // the risk manager's daily reset boundary, SQLite equity points, and any
    // timestamp-sensitive strategy.
    //
    // Deterministic mode (seed != 0) keeps the legacy synthetic stepping
    // (base_ts + i ms) so reproducibility tests and golden regressions stay
    // stable. Normal runs consult the CSV date column, falling back to
    // +1 ms stepping only when the parse fails or regresses — that covers
    // missing/malformed dates without silently reordering rows.
    const bool use_csv_dates = (config_.seed == 0);
    auto resolve_bar_ts = [&](std::size_t i,
                              const std::chrono::system_clock::time_point& prev)
        -> std::chrono::system_clock::time_point
    {
        if (use_csv_dates)
        {
            const auto& date_str = (i < data_handler_->db_data_date.size())
                ? data_handler_->db_data_date[i] : std::string{};
            if (auto parsed = tt::date_parse::parse(date_str))
            {
                if (i == 0 || *parsed > prev) return *parsed;
            }
        }
        if (i == 0)
            return base_ts + std::chrono::milliseconds(static_cast<long long>(i));
        return prev + std::chrono::milliseconds(1);
    };

    std::chrono::system_clock::time_point prev_bar_ts{};

    for (std::size_t i = 0; i < n && !halt_requested
             && !halt_flag_.load(std::memory_order_acquire)
             && !worker_failed_.load(std::memory_order_acquire); ++i)
    {
        DEBUG_STAGE(stage_timer_, market_create);
        auto this_bar_ts = resolve_bar_ts(i, prev_bar_ts);
        prev_bar_ts = this_bar_ts;
        market_event mkt(
            this_bar_ts,
            data_handler_->db_data_symbol[i],
            data_handler_->db_data_open_value[i],
            data_handler_->db_data_high_value[i],
            data_handler_->db_data_low_value[i],
            data_handler_->db_data_close_value[i],
            data_handler_->db_data_volume_value[i]
        );
        mkt.set_recv_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        auto sim_time = mkt.get_timestamp();
        const auto& symbol = mkt.get_symbol();

        last_mid_price_ = mkt.get_open();

        {
            DEBUG_STAGE(stage_timer_, pending_drain);
            while (!pending_orders_.empty() &&
                   pending_orders_.top().order->get_earliest_eligible_ts() <= sim_time)
            {
                auto entry = pending_orders_.top();
                pending_orders_.pop();
                if (entry.order->get_tif() == time_in_force::day)
                    day_order_ids_.push_back({entry.order->get_symbol(), entry.order->get_order_id()});
                if (!process_order(entry.order, event_count, halt_requested)) break;
            }
        }
        if (halt_requested) break;

        last_mid_price_ = mkt.get_close();

        {
            DEBUG_STAGE(stage_timer_, stop_check);
            check_pending_stops(mkt.get_high(), mkt.get_low(), sim_time, event_count, halt_requested);
        }

        {
            DEBUG_STAGE(stage_timer_, mm_replenish);
            auto ob = orderbook_registry_.get_or_create(symbol);
            if (!preset_has_mm_worker(config_.threading) &&
                !l2_seeded_symbols_.count(symbol))
                market_maker_.replenish(ob, last_mid_price_);
        }

        if (mm_order_ring_)
        {
            event_pointer mm_ev;
            while (mm_order_ring_->try_pop(mm_ev))
            {
                if (mm_ev->get_type() == event_type::order)
                {
                    auto& mm_order = static_cast<order_event&>(*mm_ev);
                    auto mm_ob = orderbook_registry_.get_or_create(mm_order.get_symbol());
                    auto mm_side = (mm_order.get_side() == order_side::buy) ? side::buy : side::sell;
                    auto mm_ob_order = std::make_shared<order>(
                        ob_order_type::good_till_cancel, mm_order.get_order_id(),
                        mm_side, Price::from_double(mm_order.get_price()),
                        static_cast<quantity>(std::round(mm_order.get_quantity() * 1e8)));
                    mm_ob->add_order(mm_ob_order);
                }
            }
        }

        auto mkt_ptr = market_pool_.acquire(mkt);
        std::optional<order_event> order_opt;
        {
            DEBUG_STAGE(stage_timer_, strategy);
            order_opt = strategy_->on_market(mkt);
        }
        log_event(mkt);
        {
            DEBUG_STAGE(stage_timer_, ring_publish);
            publish_event(mkt_ptr);
        }
        if (!config_.is_threaded())
            analytics_.on_event(mkt_ptr);
        event_count++;

#ifdef HAS_WEB_UI
        broadcast_market_with_indicators(mkt);

        broadcast_orderbook_snapshot(symbol);

        process_ws_commands(halt_requested, event_count);

        if (ws_worker_ && ws_worker_->has_pending_connect())
            send_state_snapshot();
#endif

        if (evaluate_exits(symbol, mkt.get_close(), sim_time,
                           event_count, mkt.get_recv_ns()))
            break;

        if (order_opt)
        {
            order_opt->set_recv_ns(mkt.get_recv_ns());
            if (!primary_strategy_name_.empty())
                order_opt->set_strategy_name(primary_strategy_name_);
            std::uint64_t oid = order_opt->get_order_id();
            route_order(*order_opt, sim_time, event_count, halt_requested);
            if (!halt_requested && strategy_)
                register_strategy_exit_intent(*strategy_, primary_strategy_name_, oid);
        }
        dispatch_extras_on_market(mkt, sim_time, event_count);

        {
            auto now_report = std::chrono::steady_clock::now();
            if ((i + 1) == n || now_report - last_report_time >= std::chrono::milliseconds(200))
            {
                const double progress = ((i + 1) * 100.0) / static_cast<double>(n);
                std::cout << "\rProgress: " << std::fixed << std::setprecision(3) << progress
                          << "% | Trades executed: " << portfolio_.get_total_trades()
                          << std::flush;
                last_report_time = now_report;
            }
        }

        write_checkpoint_if_due(event_count);
    }

#ifdef HAS_WEB_UI
    if (ws_worker_)
    {
        if (worker_failed_.load(std::memory_order_acquire))
            ws_worker_->broadcast(event_json::error_to_json("Worker thread crashed", "engine"));
        else if (halt_flag_.load(std::memory_order_acquire))
            ws_worker_->broadcast(event_json::error_to_json("Risk manager halted engine", "risk"));
    }
#endif

    while (!pending_orders_.empty())
    {
        auto entry = pending_orders_.top();
        pending_orders_.pop();
        if (entry.order->get_tif() == time_in_force::day)
            day_order_ids_.push_back({entry.order->get_symbol(), entry.order->get_order_id()});
        process_order(entry.order, event_count, halt_requested);
    }

    for (const auto& [symbol, oid] : day_order_ids_)
    {
        auto ob = orderbook_registry_.get(symbol);
        if (ob)
            ob->cancel_order(oid);
    }

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << std::endl;
    std::cout << "Trades executed: " << portfolio_.get_total_trades()
              << " in " << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

    double throughput = static_cast<double>(event_count) / (elapsed_ms / 1000.0);
    std::cout << "Event throughput: " << throughput << " events/second" << std::endl;

#ifdef HAS_SQLITE
    if (store_)
    {
        store_->flush_all();
        auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string pos_json = "[";
        bool first = true;
        for (const auto& [sym, pos] : portfolio_.get_positions())
        {
            // Skip only flat positions; keep shorts (qty < 0) so the
            // snapshot truthfully reflects end-of-run exposure.
            if (std::abs(pos.qty) < 1e-12) continue;
            if (!first) pos_json += ",";
            first = false;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                R"({"symbol":"%s","qty":%.8g,"cost_basis":%.6f})",
                sym.c_str(), pos.qty, pos.cost_basis);
            pos_json += buf;
        }
        pos_json += "]";

        store_->insert_portfolio_snapshot(
            portfolio_.get_cash(), portfolio_.get_equity(last_mid_price_),
            pos_json, portfolio_.get_total_trades(), ts_ms);
    }
    record_run_end();
#endif

    if (!config_.checkpoint_path.empty())
    {
        try {
            auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            checkpoint::write_file(config_.checkpoint_path, portfolio_,
                                   static_cast<uint64_t>(event_count), wall_ms);
        } catch (const std::exception& e) {
            std::cerr << "[checkpoint] final write failed: " << e.what() << std::endl;
        }
    }

    if (event_logger_) event_logger_->flush();
    stop_workers();

#ifdef HAS_DEBUG
    memory_sampler_.set_end(debug::memory_snapshot::capture());
    {
        debug::DebugReport report;
        std::vector<std::pair<const char*, const debug::thread_utilization*>> worker_utils;
        if (logging_worker_)    worker_utils.push_back({"logging", &logging_worker_->debug_utilization()});
        if (risk_worker_)       worker_utils.push_back({"risk", &risk_worker_->debug_utilization()});
        if (stats_worker_)      worker_utils.push_back({"stats", &stats_worker_->debug_utilization()});
        if (observer_worker_)   worker_utils.push_back({"observer", &observer_worker_->debug_utilization()});
        if (risk_stats_worker_) worker_utils.push_back({"risk_stats", &risk_stats_worker_->debug_utilization()});
        if (mm_worker_)         worker_utils.push_back({"market_maker", &mm_worker_->debug_utilization()});

        std::vector<const debug::ring_diagnostics*> ring_diags = {
            &logging_diag_, &risk_diag_, &stats_diag_,
            &observer_diag_, &risk_stats_diag_, &mm_diag_
        };

        report.log_all(stage_timer_, memory_sampler_, worker_utils, ring_diags);
    }
#endif
}

void engine::run_tick_data()
{
    if (!config_.event_log_path.empty() && !event_logger_)
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path, config_.compress_log);

#ifdef HAS_SQLITE
    if (current_run_id_.empty()) record_run_begin();
#endif

    pending_stops_.clear();
    while (!pending_orders_.empty()) pending_orders_.pop();
    order_seq_ = 0;
    day_order_ids_.clear();

    start_workers();
    pin_event_loop_thread();

    const auto& ticks = data_handler_->tick_data;
    const auto n = ticks.size();
    const auto start = std::chrono::high_resolution_clock::now();

    std::cout << "\rProgress: 0.000% | Trades executed: 0" << std::flush;

    std::size_t event_count = 0;
    bool halt_requested = false;
    auto last_report_time = std::chrono::steady_clock::now();

    BarAggregator bar_agg(std::chrono::seconds(1), [&](const market_event& bar)
    {
        int64_t bar_recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        auto bar_ptr = market_pool_.acquire(bar);
        bar_ptr->set_recv_ns(bar_recv_ns);
        auto order_opt = strategy_->on_market(bar);
        publish_event(bar_ptr);
        if (!config_.is_threaded())
            analytics_.on_event(bar_ptr);

        if (evaluate_exits(bar.get_symbol(), bar.get_close(), bar.get_timestamp(),
                           event_count, bar_recv_ns))
            return;

        if (order_opt)
        {
            order_opt->set_recv_ns(bar_recv_ns);
            if (!primary_strategy_name_.empty())
                order_opt->set_strategy_name(primary_strategy_name_);
            std::uint64_t oid = order_opt->get_order_id();
            route_order(*order_opt, bar.get_timestamp(), event_count, halt_requested);
            if (!halt_requested && strategy_)
                register_strategy_exit_intent(*strategy_, primary_strategy_name_, oid);
        }
        dispatch_extras_on_market(bar, bar.get_timestamp(), event_count);
    });

    for (std::size_t i = 0; i < n && !halt_requested
             && !halt_flag_.load(std::memory_order_acquire)
             && !worker_failed_.load(std::memory_order_acquire); ++i)
    {
        const auto& tick = ticks[i];

        last_mid_price_ = tick.price;

        auto ob = orderbook_registry_.get_or_create(tick.symbol);
        if (!l2_seeded_symbols_.count(tick.symbol))
            market_maker_.replenish(ob, last_mid_price_);

        while (!pending_orders_.empty() &&
               pending_orders_.top().order->get_earliest_eligible_ts() <= tick.timestamp)
        {
            auto entry = pending_orders_.top();
            pending_orders_.pop();
            if (entry.order->get_tif() == time_in_force::day)
                day_order_ids_.push_back({entry.order->get_symbol(), entry.order->get_order_id()});
            if (!process_order(entry.order, event_count, halt_requested)) break;
        }
        if (halt_requested) break;

        check_pending_stops(tick.price, tick.price, tick.timestamp, event_count, halt_requested);
        if (halt_requested) break;

        tick_side ts = tick_side::unknown;
        if (tick.side == data_tick_side::bid) ts = tick_side::bid;
        else if (tick.side == data_tick_side::ask) ts = tick_side::ask;

        tick_event te(tick.timestamp, tick.symbol, tick.price, tick.quantity, ts);
        te.set_recv_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        auto tick_ptr = tick_pool_.acquire(te);
        log_event(te);
        publish_event(tick_ptr);
        if (!config_.is_threaded())
            analytics_.on_event(tick_ptr);
        event_count++;

        if (evaluate_exits(tick.symbol, tick.price, tick.timestamp,
                           event_count, te.get_recv_ns()))
            break;

        auto order_opt = strategy_->on_tick(te);
        if (order_opt)
        {
            order_opt->set_recv_ns(te.get_recv_ns());
            if (!primary_strategy_name_.empty())
                order_opt->set_strategy_name(primary_strategy_name_);
            std::uint64_t oid = order_opt->get_order_id();
            route_order(*order_opt, tick.timestamp, event_count, halt_requested);
            if (!halt_requested && strategy_)
                register_strategy_exit_intent(*strategy_, primary_strategy_name_, oid);
        }
        dispatch_extras_on_tick(te, tick.timestamp, event_count);
        if (halt_requested) break;

        bar_agg.on_tick(tick.symbol, tick.price, tick.quantity, tick.timestamp);

        {
            auto now_report = std::chrono::steady_clock::now();
            if ((i + 1) == n || now_report - last_report_time >= std::chrono::milliseconds(200))
            {
                const double progress = ((i + 1) * 100.0) / static_cast<double>(n);
                std::cout << "\rProgress: " << std::fixed << std::setprecision(3) << progress
                          << "% | Trades executed: " << portfolio_.get_total_trades()
                          << std::flush;
                last_report_time = now_report;
            }
        }
    }

    bar_agg.flush();

    while (!pending_orders_.empty())
    {
        auto entry = pending_orders_.top();
        pending_orders_.pop();
        process_order(entry.order, event_count, halt_requested);
    }

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << std::endl;
    std::cout << "Trades executed: " << portfolio_.get_total_trades()
              << " in " << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

    double throughput = static_cast<double>(event_count) / (elapsed_ms / 1000.0);
    std::cout << "Event throughput: " << throughput << " events/second" << std::endl;

#ifdef HAS_SQLITE
    if (store_) store_->flush_all();
    record_run_end();
#endif
    if (event_logger_) event_logger_->flush();
    stop_workers();
}

void engine::run_replay(const std::string& log_path,
                        int64_t replay_from_us,
                        int64_t replay_to_us)
{
    EventReplayer replayer(log_path, replay_from_us, replay_to_us);

    start_workers();
    pin_event_loop_thread();

    // Replay re-runs the strategy against the logged market data, which
    // means the same routing pipeline must be used as the original run —
    // otherwise instrument_spec / execution_bar_delay / latency_model /
    // stop-order / day-order behavior diverges between the logged run and
    // its replay, defeating the point. Previously the switch below
    // reimplemented a thinner order path inline; it now delegates to
    // route_order + process_order exactly like run() and run_streaming().
    pending_stops_.clear();
    while (!pending_orders_.empty()) pending_orders_.pop();
    order_seq_ = 0;
    day_order_ids_.clear();

    const auto start = std::chrono::high_resolution_clock::now();
    std::size_t event_count = 0;
    bool halt_requested = false;

    std::cout << "\rReplay: processing events..." << std::flush;

    while (replayer.has_next())
    {
        auto ev = replayer.next();
        if (!ev) break;

        if (halt_requested || halt_flag_.load(std::memory_order_acquire))
            break;

        switch (ev->get_type()) {
        case event_type::market: {
            auto& mkt = static_cast<market_event&>(*ev);
            const auto sim_time = mkt.get_timestamp();

            last_mid_price_ = mkt.get_open();

            while (!pending_orders_.empty() &&
                   pending_orders_.top().order->get_earliest_eligible_ts() <= sim_time)
            {
                auto entry = pending_orders_.top();
                pending_orders_.pop();
                if (entry.order->get_tif() == time_in_force::day)
                    day_order_ids_.push_back({entry.order->get_symbol(), entry.order->get_order_id()});
                if (!process_order(entry.order, event_count, halt_requested)) break;
            }
            if (halt_requested) break;

            last_mid_price_ = mkt.get_close();

            check_pending_stops(mkt.get_high(), mkt.get_low(), sim_time,
                                event_count, halt_requested);
            if (halt_requested) break;

            auto ob = orderbook_registry_.get_or_create(mkt.get_symbol());
            if (!l2_seeded_symbols_.count(mkt.get_symbol()))
                market_maker_.replenish(ob, last_mid_price_);

            auto order_opt = strategy_->on_market(mkt);
            publish_event(ev);
            if (!config_.is_threaded())
                analytics_.on_event(ev);

            if (evaluate_exits(mkt.get_symbol(), mkt.get_close(), sim_time,
                               event_count, mkt.get_recv_ns()))
                break;

            if (order_opt)
            {
                if (!primary_strategy_name_.empty())
                    order_opt->set_strategy_name(primary_strategy_name_);
                order_opt->set_recv_ns(mkt.get_recv_ns());
                std::uint64_t oid = order_opt->get_order_id();
                route_order(*order_opt, sim_time, event_count, halt_requested);
                if (!halt_requested && strategy_)
                    register_strategy_exit_intent(*strategy_, primary_strategy_name_, oid);
            }
            dispatch_extras_on_market(mkt, sim_time, event_count);
            break;
        }
        case event_type::tick: {
            auto& te = static_cast<tick_event&>(*ev);
            const auto sim_time = te.get_timestamp();

            last_mid_price_ = te.get_price();

            while (!pending_orders_.empty() &&
                   pending_orders_.top().order->get_earliest_eligible_ts() <= sim_time)
            {
                auto entry = pending_orders_.top();
                pending_orders_.pop();
                if (entry.order->get_tif() == time_in_force::day)
                    day_order_ids_.push_back({entry.order->get_symbol(), entry.order->get_order_id()});
                if (!process_order(entry.order, event_count, halt_requested)) break;
            }
            if (halt_requested) break;

            check_pending_stops(te.get_price(), te.get_price(), sim_time,
                                event_count, halt_requested);
            if (halt_requested) break;

            publish_event(ev);
            if (!config_.is_threaded())
                analytics_.on_event(ev);

            if (evaluate_exits(te.get_symbol(), te.get_price(), sim_time,
                               event_count, te.get_recv_ns()))
                break;

            auto order_opt = strategy_->on_tick(te);
            if (order_opt)
            {
                if (!primary_strategy_name_.empty())
                    order_opt->set_strategy_name(primary_strategy_name_);
                order_opt->set_recv_ns(te.get_recv_ns());
                std::uint64_t oid = order_opt->get_order_id();
                route_order(*order_opt, sim_time, event_count, halt_requested);
                if (!halt_requested && strategy_)
                    register_strategy_exit_intent(*strategy_, primary_strategy_name_, oid);
            }
            dispatch_extras_on_tick(te, sim_time, event_count);
            break;
        }
        default:
            // Non-market/non-tick events (orders, fills, cancels, rejections)
            // from the original log are re-published for observers but NOT
            // re-executed — replay regenerates orders by re-running the
            // strategy on the market data.
            publish_event(ev);
            if (!config_.is_threaded())
                analytics_.on_event(ev);
            break;
        }

        event_count++;
    }

    // Drain any pending orders still eligible at end-of-log so we don't
    // leak queued state across replays.
    while (!pending_orders_.empty())
    {
        auto entry = pending_orders_.top();
        pending_orders_.pop();
        if (entry.order->get_tif() == time_in_force::day)
            day_order_ids_.push_back({entry.order->get_symbol(), entry.order->get_order_id()});
        process_order(entry.order, event_count, halt_requested);
    }
    for (const auto& [symbol, oid] : day_order_ids_)
    {
        auto ob = orderbook_registry_.get(symbol);
        if (ob) ob->cancel_order(oid);
    }

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << std::endl;
    std::cout << "Replay complete: " << event_count << " events in "
              << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

    stop_workers();
}
