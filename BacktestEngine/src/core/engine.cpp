#include "engine.h"
#include "../data/data_handler.h"
#include "../execution/portfolio.h"
#include "../execution/fee_model.h"
#include "../execution/latency_model.h"
#include "../orderbook/fill_model.h"
#include "../providers/provider.h"

#include <iostream>
#include <vector>
#include <queue>
#include <chrono>
#include <iomanip>
#include <algorithm>

engine::engine(std::shared_ptr<data_handler> dh,
               std::shared_ptr<orderbook> ob,
               std::shared_ptr<IStrategy> strategy,
               engine_config config)
    : config_(std::move(config)), data_handler_(std::move(dh)), strategy_(std::move(strategy)),
      portfolio_(config_.initial_balance),
      risk_manager_(config_.risk),
      market_maker_(config_.seed != 0 ? MarketMaker(static_cast<unsigned>(config_.seed + 1))
                                      : MarketMaker())
{
    // Register the provided orderbook under a default symbol
    if (ob)
        orderbook_registry_ = OrderbookRegistry();

    // Shadow mode: create tracker for simulated vs exchange fill comparison
    if (config_.mode == engine_mode::shadow)
        shadow_tracker_ = std::make_unique<ShadowTracker>();
}

std::shared_ptr<IExecutionAdapter> engine::get_adapter(const std::string& symbol)
{
    auto it = execution_adapters_.find(symbol);
    if (it != execution_adapters_.end())
        return it->second;

    auto ob = orderbook_registry_.get_or_create(symbol);

    std::shared_ptr<IExecutionAdapter> adapter;
    if (config_.mode == engine_mode::live && config_.provider &&
        config_.provider->get_execution_adapter())
    {
        // Live mode: use provider's execution adapter (e.g. Binance REST API)
        adapter = config_.provider->get_execution_adapter();
    }
    else
    {
        // Backtest + shadow mode: use local orderbook adapter
        adapter = std::make_shared<LocalBookAdapter>(
            ob, config_.fee_model, config_.fill_model,
            config_.seed != 0 ? static_cast<unsigned>(config_.seed + 2) : 42u);
    }

    execution_adapters_[symbol] = adapter;
    return adapter;
}

void engine::log_event(const event& ev)
{
    // When threaded, logging happens in the logging/observer worker
    if (config_.is_threaded())
        return;
    if (event_logger_)
        event_logger_->log(ev);
}

void engine::publish_event(const event_pointer& ev)
{
    switch (config_.threading)
    {
    case thread_preset::inline_mode:
        break;

    case thread_preset::light:
        if (observer_ring_ && !observer_ring_->try_push(ev)) { observer_drops_++;
#ifdef HAS_DEBUG
            observer_diag_.on_drop();
#endif
        }
#ifdef HAS_DEBUG
        else if (observer_ring_) observer_diag_.on_push(observer_ring_->occupancy());
#endif
        break;

    case thread_preset::standard:
        if (logging_ring_ && !logging_ring_->try_push(ev)) { logging_drops_++;
#ifdef HAS_DEBUG
            logging_diag_.on_drop();
#endif
        }
#ifdef HAS_DEBUG
        else if (logging_ring_) logging_diag_.on_push(logging_ring_->occupancy());
#endif
        if (risk_stats_ring_ && !risk_stats_ring_->try_push(ev)) { risk_stats_drops_++;
#ifdef HAS_DEBUG
            risk_stats_diag_.on_drop();
#endif
        }
#ifdef HAS_DEBUG
        else if (risk_stats_ring_) risk_stats_diag_.on_push(risk_stats_ring_->occupancy());
#endif
        break;

    case thread_preset::full:
        if (logging_ring_ && !logging_ring_->try_push(ev)) { logging_drops_++;
#ifdef HAS_DEBUG
            logging_diag_.on_drop();
#endif
        }
#ifdef HAS_DEBUG
        else if (logging_ring_) logging_diag_.on_push(logging_ring_->occupancy());
#endif
        if (risk_ring_ && !risk_ring_->try_push(ev)) { risk_drops_++;
#ifdef HAS_DEBUG
            risk_diag_.on_drop();
#endif
        }
#ifdef HAS_DEBUG
        else if (risk_ring_) risk_diag_.on_push(risk_ring_->occupancy());
#endif
        if (stats_ring_ && !stats_ring_->try_push(ev)) { stats_drops_++;
#ifdef HAS_DEBUG
            stats_diag_.on_drop();
#endif
        }
#ifdef HAS_DEBUG
        else if (stats_ring_) stats_diag_.on_push(stats_ring_->occupancy());
#endif
        break;

    case thread_preset::extended:
        if (logging_ring_ && !logging_ring_->try_push(ev)) { logging_drops_++;
#ifdef HAS_DEBUG
            logging_diag_.on_drop();
#endif
        }
#ifdef HAS_DEBUG
        else if (logging_ring_) logging_diag_.on_push(logging_ring_->occupancy());
#endif
        if (risk_ring_ && !risk_ring_->try_push(ev)) { risk_drops_++;
#ifdef HAS_DEBUG
            risk_diag_.on_drop();
#endif
        }
#ifdef HAS_DEBUG
        else if (risk_ring_) risk_diag_.on_push(risk_ring_->occupancy());
#endif
        if (stats_ring_ && !stats_ring_->try_push(ev)) { stats_drops_++;
#ifdef HAS_DEBUG
            stats_diag_.on_drop();
#endif
        }
#ifdef HAS_DEBUG
        else if (stats_ring_) stats_diag_.on_push(stats_ring_->occupancy());
#endif
        if (mm_ring_ && !mm_ring_->try_push(ev)) { mm_drops_++;
#ifdef HAS_DEBUG
            mm_diag_.on_drop();
#endif
        }
#ifdef HAS_DEBUG
        else if (mm_ring_) mm_diag_.on_push(mm_ring_->occupancy());
#endif
        break;
    }

#ifdef HAS_WEB_UI
    if (ws_ring_ && !ws_ring_->try_push(ev))
        ws_drops_++;
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
        config_.event_log_path, text_sink, config_.text_log_path);
}

void engine::start_workers()
{
    halt_flag_.store(false, std::memory_order_release);
    worker_failed_.store(false, std::memory_order_release);

    // Helper to wire shared failure flag to any worker
    auto wire_failure = [this](Worker& w) { w.set_failure_flag(worker_failed_); };

#ifdef HAS_WEB_UI
    // Start WebSocket worker if enabled (works with any threading preset)
    if (config_.enable_web_ui)
    {
        ws_ring_ = std::make_shared<EventRing>();
        ws_worker_ = std::make_unique<WebSocketWorker>(config_.ws_port);
        wire_failure(*ws_worker_);

        worker_threads_.emplace_back([this]() {
            ws_worker_->run(*ws_ring_);
        });
    }
#endif

    if (!config_.is_threaded())
        return;

    // Build core map for pinning
    auto core_map = build_core_map();

    auto find_core = [&](core_role role) -> int {
        if (config_.disable_pinning) return -1;
        // Check explicit overrides first
        switch (role) {
        case core_role::event_loop: if (config_.pin_event_loop >= 0) return config_.pin_event_loop; break;
        case core_role::logging:    if (config_.pin_logging >= 0)    return config_.pin_logging; break;
        case core_role::risk:       if (config_.pin_risk >= 0)       return config_.pin_risk; break;
        case core_role::stats:      if (config_.pin_stats >= 0)      return config_.pin_stats; break;
        case core_role::market_maker: if (config_.pin_mm >= 0)       return config_.pin_mm; break;
        }
        // Fall back to auto-detected core map
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
        observer_worker_ = std::make_unique<ObserverWorker>(risk_manager_, halt_flag_);
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
        risk_stats_worker_ = std::make_unique<RiskStatsWorker>(risk_manager_, halt_flag_);
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
        risk_worker_ = std::make_unique<RiskWorker>(risk_manager_, halt_flag_);
        stats_worker_ = std::make_unique<StatsWorker>();
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
        // Full + market maker worker
        logging_ring_ = std::make_shared<EventRing>();
        risk_ring_ = std::make_shared<EventRing>();
        stats_ring_ = std::make_shared<EventRing>();
        mm_ring_ = std::make_shared<EventRing>();
        mm_order_ring_ = std::make_shared<MMRing>();

        logging_worker_ = make_logging_worker();
        risk_worker_ = std::make_unique<RiskWorker>(risk_manager_, halt_flag_);
        stats_worker_ = std::make_unique<StatsWorker>();
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
    // Signal all active workers to stop
    if (observer_worker_) observer_worker_->stop();
    if (logging_worker_) logging_worker_->stop();
    if (risk_worker_) risk_worker_->stop();
    if (stats_worker_) stats_worker_->stop();
    if (risk_stats_worker_) risk_stats_worker_->stop();
    if (mm_worker_) mm_worker_->stop();
#ifdef HAS_WEB_UI
    if (ws_worker_) ws_worker_->stop();
#endif

    // Join all threads
    for (auto& t : worker_threads_)
    {
        if (t.joinable())
            t.join();
    }
    worker_threads_.clear();

    // Drain any remaining events from MM inbound ring to avoid
    // use-after-free when the ring outlives the worker's object pool.
    if (mm_order_ring_)
    {
        event_pointer ev;
        while (mm_order_ring_->try_pop(ev)) {}
    }

    // Report worker exceptions
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

    // Report drop counts
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
        else if (cmd.command == "order" && !cmd.side.empty() && cmd.quantity > 0.0)
        {
            // Build and process an order from the UI
            auto otype = (cmd.order_type == "limit") ? order_type::limit : order_type::market;
            auto oside = (cmd.side == "sell") ? order_side::sell : order_side::buy;
            double price = (otype == order_type::limit && cmd.price > 0.0) ? cmd.price : last_mid_price_;

            // Use the first known symbol, or empty string
            std::string symbol;
            if (!data_handler_->db_data_symbol.empty())
                symbol = data_handler_->db_data_symbol.back();

            auto ts = std::chrono::system_clock::now();
            auto o = order_pool_.acquire(ts, symbol, otype, oside, cmd.quantity, price, time_in_force::ioc);
            o->set_order_id(OrderIdGenerator::next());
            o->set_earliest_eligible_ts(ts);

            process_order(o, event_count, halt_requested);
        }
        else if (cmd.command == "set_timeframe" && !cmd.timeframe.empty())
        {
            // Parse timeframe string (e.g. "1m", "5m", "15m", "1h") to milliseconds
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

                    // Flush current aggregator and recreate with new interval
                    if (tick_aggregator_)
                    {
                        tick_aggregator_->flush();
                        tick_aggregator_ = std::make_unique<BarAggregator>(
                            tick_bar_interval_,
                            [this](const market_event& bar) {
                                broadcast_market_with_indicators(bar);
                            });
                    }

                    // Tell the UI to clear its chart data
                    ws_worker_->broadcast(R"({"type":"chart_reset","data":{"timeframe":")"
                        + cmd.timeframe + R"("}})");
                }
            }
        }
        // "start" and "pause" are informational — the engine is already running in its loop
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

    // 1. Engine status
    ws_worker_->broadcast_status("running",
        config_.provider ? config_.provider->name() : "",
        (!data_handler_->db_data_symbol.empty()) ? data_handler_->db_data_symbol.back() : "");

    // 2. Portfolio snapshot
    {
        auto json = event_json::portfolio_to_json(portfolio_);
        ws_worker_->broadcast(json);
    }

    // 3. Analytics snapshot
    {
        auto report = analytics_.snapshot();
        report.final_equity = portfolio_.get_equity(last_mid_price_);
        auto json = event_json::analytics_to_json(report);
        ws_worker_->broadcast(json);
    }
}

void engine::broadcast_market_with_indicators(const market_event& mkt)
{
    if (!ws_worker_) return;

    auto indicators = strategy_ ? strategy_->get_indicator_values(mkt.get_symbol())
                                : std::vector<std::pair<std::string, double>>{};

    // Broadcast market event (with indicators when available) via WS worker
    auto json = event_json::to_json_with_indicators(mkt, indicators);
    ws_worker_->broadcast(json);
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

    // Shadow mode: print comparison report
    if (shadow_tracker_)
        shadow_tracker_->print_report();
}

bool engine::process_order(const std::shared_ptr<order_event>& o,
                           std::size_t& event_count,
                           bool& halt_requested)
{
    // Pre-order risk check (inline only — when threaded, RiskWorker handles this)
    if (!config_.is_threaded())
    {
        auto snap = analytics_.snapshot();
        auto action = risk_manager_.check_order(*o, portfolio_, snap);
        if (action == risk_action::halt)
        {
            halt_requested = true;
            return false;
        }
        if (action == risk_action::reject)
            return true; // drop order, continue engine
    }

    log_event(*o);
    publish_event(o);
    if (!config_.is_threaded())
        analytics_.on_event(o);

    auto adapter = get_adapter(o->get_symbol());

    // Update mid price on the adapter (for fill model distance calculations)
    if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
        local->set_mid_price(last_mid_price_);

    adapter->submit_order(*o);

    // Shadow mode: also forward order to provider's execution adapter
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
            auto fill_ptr = fill_pool_.acquire(f);
            log_event(f);
            portfolio_.on_fill(f);
            strategy_->set_position_open(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
            publish_event(fill_ptr);
            if (!config_.is_threaded())
                analytics_.on_event(fill_ptr);

            // Shadow mode: track simulated fill for comparison
            if (config_.mode == engine_mode::shadow && shadow_tracker_)
                shadow_tracker_->on_simulated_fill(f);

            event_count++;

            // Post-fill risk check (inline only)
            if (!config_.is_threaded())
            {
                auto post_snap = analytics_.generate_report();
                auto post_action = risk_manager_.check_post_fill(f, portfolio_, post_snap);
                if (post_action == risk_action::halt)
                {
                    halt_requested = true;
                    return false;
                }
            }
        }
    }

    // Shadow mode: poll exchange fills for comparison
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

    last_mid_price_ = mkt.get_close();

    // Replenish orderbook for this symbol
    auto ob = orderbook_registry_.get_or_create(mkt.get_symbol());
    if (!preset_has_mm_worker(config_.threading))
        market_maker_.replenish(ob, last_mid_price_);

    // Process market event through strategy
    auto mkt_ptr = market_pool_.acquire(mkt);
    auto order_opt = strategy_->on_market(mkt);
    log_event(mkt);
    publish_event(mkt_ptr);
    if (!config_.is_threaded())
        analytics_.on_event(mkt_ptr);
    event_count++;

#ifdef HAS_WEB_UI
    broadcast_market_with_indicators(mkt);
#endif

    if (order_opt)
    {
        order_opt->set_order_id(OrderIdGenerator::next());
        order_opt->set_earliest_eligible_ts(timestamp);
        bool halt = false;
        process_order(order_pool_.acquire(*order_opt), event_count, halt);
    }
}

void engine::process_single_tick(const tick_record& rec, std::size_t& event_count)
{
    tick_side ts = tick_side::unknown;
    if (rec.side == data_tick_side::bid) ts = tick_side::bid;
    else if (rec.side == data_tick_side::ask) ts = tick_side::ask;

    tick_event te(rec.timestamp, rec.symbol, rec.price, rec.quantity, ts);

    last_mid_price_ = rec.price;

    // Replenish orderbook with liquidity around current price.
    // Always inline for tick data — the MM worker only handles bar-driven replenish.
    auto ob = orderbook_registry_.get_or_create(rec.symbol);
    market_maker_.replenish(ob, last_mid_price_);

    auto tick_ptr = tick_pool_.acquire(te);
    log_event(te);
    publish_event(tick_ptr);
    if (!config_.is_threaded())
        analytics_.on_event(tick_ptr);
    event_count++;

#ifdef HAS_WEB_UI
    // Feed tick into bar aggregator for charting (emits bars via callback)
    if (tick_aggregator_)
        tick_aggregator_->on_tick(rec.symbol, rec.price, rec.quantity, rec.timestamp);
#endif

    // Dispatch to strategy's tick handler
    auto order_opt = strategy_->on_tick(te);
    if (order_opt)
    {
        order_opt->set_order_id(OrderIdGenerator::next());
        order_opt->set_earliest_eligible_ts(rec.timestamp);

        auto adapter = get_adapter(rec.symbol);
        if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
            local->set_mid_price(last_mid_price_);
        adapter->submit_order(*order_opt);

        auto order_ptr = order_pool_.acquire(*order_opt);
        publish_event(order_ptr);
        if (!config_.is_threaded())
            analytics_.on_event(order_ptr);

        std::vector<fill_event> fills;
        if (adapter->poll_fills(fills))
        {
            for (auto& f : fills)
            {
                auto fill_ptr = fill_pool_.acquire(f);
                portfolio_.on_fill(f);
                strategy_->set_position_open(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                publish_event(fill_ptr);
                if (!config_.is_threaded())
                    analytics_.on_event(fill_ptr);
                event_count++;
            }
        }
        event_count++;
    }
}

void engine::run_streaming(std::shared_ptr<DataBridge<bar_record>> bridge)
{
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    if (!config_.event_log_path.empty())
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path);

    start_workers();

    const auto start = std::chrono::high_resolution_clock::now();
    std::size_t event_count = 0;
    std::size_t bar_index = 0;

    std::cout << "\rStreaming: waiting for data..." << std::flush;

    auto last_report_time = std::chrono::steady_clock::now();

    bridge->run_streaming(data_handler_, [&](const bar_record& rec) {
        // Use the bar's own timestamp when available (e.g. Binance kline open time
        // stored as epoch-ms string in rec.date), fall back to wall clock.
        auto timestamp = [&]() {
            if (!rec.date.empty()) {
                try {
                    int64_t ts_ms = std::stoll(rec.date);
                    if (ts_ms > 1000000000000LL) // looks like epoch ms
                        return std::chrono::system_clock::time_point(
                            std::chrono::milliseconds(ts_ms));
                } catch (...) {}
            }
            return std::chrono::system_clock::now();
        }();
        process_single_bar(rec, event_count, timestamp);
        bar_index++;

#ifdef HAS_WEB_UI
        // Broadcast orderbook + WS commands in streaming mode
        broadcast_orderbook_snapshot(rec.symbol);
        bool halt = false;
        process_ws_commands(halt, event_count);
        if (ws_worker_ && ws_worker_->has_pending_connect())
            send_state_snapshot();
#endif

        auto now_report = std::chrono::steady_clock::now();
        if (now_report - last_report_time >= std::chrono::milliseconds(200))
        {
            std::cout << "\rStreaming: " << bar_index << " bars | Trades: "
                      << portfolio_.get_total_trades() << std::flush;
            last_report_time = now_report;
        }
    });

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << std::endl;
    std::cout << "Streaming complete: " << bar_index << " bars, "
              << portfolio_.get_total_trades() << " trades in "
              << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

    if (event_logger_) event_logger_->flush();
    stop_workers();
}

void engine::run_streaming(std::shared_ptr<DataBridge<tick_record>> bridge)
{
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    if (!config_.event_log_path.empty())
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path);

    // Create tick-to-bar aggregator for WebSocket UI charting
#ifdef HAS_WEB_UI
    tick_aggregator_ = std::make_unique<BarAggregator>(
        tick_bar_interval_,
        [this](const market_event& bar) {
            broadcast_market_with_indicators(bar);
        });
#endif

    start_workers();

    const auto start = std::chrono::high_resolution_clock::now();
    std::size_t event_count = 0;
    std::size_t tick_count = 0;

    std::cout << "\rStreaming: waiting for data..." << std::flush;

    auto last_report_time = std::chrono::steady_clock::now();

    bridge->run_streaming(data_handler_, [&](const tick_record& rec) {
        process_single_tick(rec, event_count);
        tick_count++;

#ifdef HAS_WEB_UI
        broadcast_orderbook_snapshot(rec.symbol);
        bool halt = false;
        process_ws_commands(halt, event_count);
        if (ws_worker_ && ws_worker_->has_pending_connect())
            send_state_snapshot();
#endif

        auto now_report = std::chrono::steady_clock::now();
        if (now_report - last_report_time >= std::chrono::milliseconds(200))
        {
            std::cout << "\rStreaming: " << tick_count << " ticks | Trades: "
                      << portfolio_.get_total_trades() << std::flush;
            last_report_time = now_report;
        }
    });

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << std::endl;
    std::cout << "Streaming complete: " << tick_count << " ticks, "
              << portfolio_.get_total_trades() << " trades in "
              << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

#ifdef HAS_WEB_UI
    if (tick_aggregator_) tick_aggregator_->flush();
#endif

    if (event_logger_) event_logger_->flush();
    stop_workers();
}

void engine::run()
{
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    // Dispatch to tick-based loop if tick data is available
    if (data_handler_->has_tick_data())
    {
        run_tick_data();
        return;
    }

    if (!data_handler_->has_bar_data()) {
        throw std::runtime_error("no data loaded — call IDataSource::load_data() before run()");
    }

    if (!config_.event_log_path.empty())
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path);

#ifdef HAS_DEBUG
    memory_sampler_.set_start(debug::memory_snapshot::capture());
#endif

    start_workers();

#ifdef HAS_WEB_UI
    if (ws_worker_)
    {
        std::string sym = (!data_handler_->db_data_symbol.empty()) ? data_handler_->db_data_symbol[0] : "";
        ws_worker_->broadcast_status("running", "", sym);
    }
#endif

    // Use a fixed epoch when seed is set for deterministic replay
    const auto base_ts = (config_.seed != 0)
        ? std::chrono::system_clock::time_point(std::chrono::milliseconds(0))
        : std::chrono::system_clock::now();
    const auto n = data_handler_->db_data_symbol.size();
    const auto start = std::chrono::high_resolution_clock::now();
    std::cout << "\rProgress: 0.000% | Trades executed: 0" << std::flush;

    std::size_t event_count = 0;
    auto last_report_time = std::chrono::steady_clock::now();

    // Pending stop orders: triggered when market price crosses stop_price
    std::vector<std::shared_ptr<order_event>> pending_stops;

    // Pending orders min-heap: sorted by (eligible time, sequence number for FIFO)
    struct pending_entry
    {
        std::shared_ptr<order_event> order;
        uint64_t seq;
    };

    auto pending_cmp = [](const pending_entry& a, const pending_entry& b)
    {
        if (a.order->get_earliest_eligible_ts() != b.order->get_earliest_eligible_ts())
            return a.order->get_earliest_eligible_ts() > b.order->get_earliest_eligible_ts();
        return a.seq > b.seq;
    };

    std::priority_queue<pending_entry, std::vector<pending_entry>, decltype(pending_cmp)> pending_orders(pending_cmp);
    uint64_t order_seq = 0;

    // Track day orders for session-end cancellation: (symbol, order_id)
    std::vector<std::pair<std::string, uint64_t>> day_order_ids;

    bool halt_requested = false;

    // Wraps member process_order with day-order tracking
    auto do_process_order = [&](const std::shared_ptr<order_event>& o) -> bool
    {
        if (o->get_tif() == time_in_force::day)
            day_order_ids.push_back({o->get_symbol(), o->get_order_id()});
        return process_order(o, event_count, halt_requested);
    };

    for (std::size_t i = 0; i < n && !halt_requested
             && !halt_flag_.load(std::memory_order_acquire)
             && !worker_failed_.load(std::memory_order_acquire); ++i)
    {
        DEBUG_STAGE(stage_timer_, market_create);
        market_event mkt(
            base_ts + std::chrono::milliseconds(static_cast<long long>(i)),
            data_handler_->db_data_symbol[i],
            data_handler_->db_data_open_value[i],
            data_handler_->db_data_high_value[i],
            data_handler_->db_data_low_value[i],
            data_handler_->db_data_close_value[i],
            data_handler_->db_data_volume_value[i]
        );

        auto sim_time = mkt.get_timestamp();
        const auto& symbol = mkt.get_symbol();

        // Drain eligible pending orders before processing this market event
        {
            DEBUG_STAGE(stage_timer_, pending_drain);
            while (!pending_orders.empty() &&
                   pending_orders.top().order->get_earliest_eligible_ts() <= sim_time)
            {
                auto entry = pending_orders.top();
                pending_orders.pop();
                if (!do_process_order(entry.order)) break;
            }
        }
        if (halt_requested) break;

        // Track mid price for fill model distance calculations
        last_mid_price_ = mkt.get_close();

        // Check pending stop orders for triggers
        {
            DEBUG_STAGE(stage_timer_, stop_check);
            auto it = pending_stops.begin();
            while (it != pending_stops.end())
            {
                auto& stop = *it;
                bool triggered = false;

                if (stop->get_side() == order_side::buy && mkt.get_high() >= stop->get_stop_price())
                    triggered = true;
                else if (stop->get_side() == order_side::sell && mkt.get_low() <= stop->get_stop_price())
                    triggered = true;

                if (triggered)
                {
                    if (stop->get_order_type() == order_type::stop)
                    {
                        // Convert to market order
                        auto market_order = order_pool_.acquire(
                            sim_time, stop->get_symbol(), order_type::market,
                            stop->get_side(), stop->get_quantity(), stop->get_stop_price(),
                            time_in_force::ioc);
                        market_order->set_order_id(stop->get_order_id());
                        market_order->set_earliest_eligible_ts(sim_time);
                        if (!do_process_order(market_order)) break;
                    }
                    else // stop_limit
                    {
                        // Convert to limit order at the limit price
                        auto limit_order = order_pool_.acquire(
                            sim_time, stop->get_symbol(), order_type::limit,
                            stop->get_side(), stop->get_quantity(), stop->get_price(),
                            stop->get_tif());
                        limit_order->set_order_id(stop->get_order_id());
                        limit_order->set_earliest_eligible_ts(sim_time);
                        if (!do_process_order(limit_order)) break;
                    }
                    it = pending_stops.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // Reactive market maker: replenish depleted book levels per symbol
        {
            DEBUG_STAGE(stage_timer_, mm_replenish);
            auto ob = orderbook_registry_.get_or_create(symbol);
            if (!preset_has_mm_worker(config_.threading))
                market_maker_.replenish(ob, last_mid_price_);
        }

        // Drain market maker inbound orders (extended preset only)
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

        // Process market event
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
        // Broadcast indicator-enriched market event to WS clients
        broadcast_market_with_indicators(mkt);

        // Broadcast orderbook depth snapshot (throttled to 250ms)
        broadcast_orderbook_snapshot(symbol);

        // Process inbound WS commands
        process_ws_commands(halt_requested, event_count);

        // Send state snapshot if a new client just connected
        if (ws_worker_ && ws_worker_->has_pending_connect())
            send_state_snapshot();
#endif

        if (order_opt)
        {
            order_opt->set_order_id(OrderIdGenerator::next());

            // Stop/stop-limit orders are buffered until triggered
            if (order_opt->get_order_type() == order_type::stop ||
                order_opt->get_order_type() == order_type::stop_limit)
            {
                pending_stops.push_back(order_pool_.acquire(*order_opt));
            }
            else if (config_.latency_model)
            {
                auto latency = config_.latency_model->get_order_latency();
                order_opt->set_earliest_eligible_ts(sim_time + latency);
                pending_orders.push({order_pool_.acquire(*order_opt), order_seq++});
            }
            else
            {
                // No latency model — process immediately (current behavior)
                order_opt->set_earliest_eligible_ts(sim_time);
                do_process_order(order_pool_.acquire(*order_opt));
            }
        }

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

    // Drain any remaining pending orders after all market data
    while (!pending_orders.empty())
    {
        auto entry = pending_orders.top();
        pending_orders.pop();
        do_process_order(entry.order);
    }

    // Cancel all day orders at session end
    for (const auto& [symbol, oid] : day_order_ids)
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
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path);

    start_workers();

    const auto& ticks = data_handler_->tick_data;
    const auto n = ticks.size();
    const auto start = std::chrono::high_resolution_clock::now();

    std::cout << "\rProgress: 0.000% | Trades executed: 0" << std::flush;

    std::size_t event_count = 0;
    auto last_report_time = std::chrono::steady_clock::now();

    // Bar aggregator: converts ticks into OHLCV bars for on_market()
    BarAggregator bar_agg(std::chrono::seconds(1), [&](const market_event& bar)
    {
        auto bar_ptr = market_pool_.acquire(bar);
        auto order_opt = strategy_->on_market(bar);
        publish_event(bar_ptr);
        if (!config_.is_threaded())
            analytics_.on_event(bar_ptr);

        if (order_opt)
        {
            order_opt->set_order_id(OrderIdGenerator::next());
            order_opt->set_earliest_eligible_ts(bar.get_timestamp());

            auto adapter = get_adapter(bar.get_symbol());

            if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
                local->set_mid_price(last_mid_price_);

            adapter->submit_order(*order_opt);

            auto order_ptr = order_pool_.acquire(*order_opt);
            publish_event(order_ptr);
            if (!config_.is_threaded())
                analytics_.on_event(order_ptr);

            std::vector<fill_event> fills;
            if (adapter->poll_fills(fills))
            {
                for (auto& f : fills)
                {
                    auto fill_ptr = fill_pool_.acquire(f);
                    portfolio_.on_fill(f);
                    strategy_->set_position_open(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                    publish_event(fill_ptr);
                    if (!config_.is_threaded())
                        analytics_.on_event(fill_ptr);
                    event_count++;
                }
            }
            event_count++;
        }
    });

    for (std::size_t i = 0; i < n; ++i)
    {
        const auto& tick = ticks[i];

        // Convert data_tick_side to tick_side for event
        tick_side ts = tick_side::unknown;
        if (tick.side == data_tick_side::bid) ts = tick_side::bid;
        else if (tick.side == data_tick_side::ask) ts = tick_side::ask;

        tick_event te(tick.timestamp, tick.symbol, tick.price, tick.quantity, ts);

        // Track mid price
        last_mid_price_ = tick.price;

        // Publish tick event
        auto tick_ptr = tick_pool_.acquire(te);
        log_event(te);
        publish_event(tick_ptr);
        if (!config_.is_threaded())
            analytics_.on_event(tick_ptr);
        event_count++;

        // Dispatch to strategy's tick handler
        auto order_opt = strategy_->on_tick(te);
        if (order_opt)
        {
            order_opt->set_order_id(OrderIdGenerator::next());
            order_opt->set_earliest_eligible_ts(tick.timestamp);

            auto adapter = get_adapter(tick.symbol);

            if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
                local->set_mid_price(last_mid_price_);

            adapter->submit_order(*order_opt);

            auto order_ptr = order_pool_.acquire(*order_opt);
            publish_event(order_ptr);
            if (!config_.is_threaded())
                analytics_.on_event(order_ptr);

            std::vector<fill_event> fills;
            if (adapter->poll_fills(fills))
            {
                for (auto& f : fills)
                {
                    auto fill_ptr = fill_pool_.acquire(f);
                    portfolio_.on_fill(f);
                    strategy_->set_position_open(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                    publish_event(fill_ptr);
                    if (!config_.is_threaded())
                        analytics_.on_event(fill_ptr);
                    event_count++;
                }
            }
            event_count++;
        }

        // Feed tick into bar aggregator for bar-based strategies
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

    // Flush any partial bar
    bar_agg.flush();

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << std::endl;
    std::cout << "Trades executed: " << portfolio_.get_total_trades()
              << " in " << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

    double throughput = static_cast<double>(event_count) / (elapsed_ms / 1000.0);
    std::cout << "Event throughput: " << throughput << " events/second" << std::endl;

    if (event_logger_) event_logger_->flush();
    stop_workers();
}

void engine::run_replay(const std::string& log_path)
{
    EventReplayer replayer(log_path);

    start_workers();

    const auto start = std::chrono::high_resolution_clock::now();
    std::size_t event_count = 0;

    std::cout << "\rReplay: processing events..." << std::flush;

    while (replayer.has_next())
    {
        auto ev = replayer.next();
        if (!ev) break;

        if (halt_flag_.load(std::memory_order_acquire))
            break;

        switch (ev->get_type()) {
        case event_type::market: {
            auto& mkt = static_cast<market_event&>(*ev);
            last_mid_price_ = mkt.get_close();

            // Replenish book
            auto ob = orderbook_registry_.get_or_create(mkt.get_symbol());
            market_maker_.replenish(ob, last_mid_price_);

            // Feed to strategy
            auto order_opt = strategy_->on_market(mkt);
            publish_event(ev);
            if (!config_.is_threaded())
                analytics_.on_event(ev);

            if (order_opt)
            {
                order_opt->set_order_id(OrderIdGenerator::next());
                order_opt->set_earliest_eligible_ts(mkt.get_timestamp());
                auto order_ptr = order_pool_.acquire(*order_opt);

                // Risk check
                auto snap = analytics_.snapshot();
                auto action = risk_manager_.check_order(*order_opt, portfolio_, snap);
                if (action == risk_action::reject) break;
                if (action == risk_action::halt) {
                    halt_flag_.store(true, std::memory_order_release);
                    break;
                }

                publish_event(order_ptr);
                if (!config_.is_threaded())
                    analytics_.on_event(order_ptr);

                auto adapter = get_adapter(mkt.get_symbol());
                if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
                    local->set_mid_price(last_mid_price_);
                adapter->submit_order(*order_opt);

                std::vector<fill_event> fills;
                if (adapter->poll_fills(fills))
                {
                    for (auto& f : fills)
                    {
                        auto fill_ptr = fill_pool_.acquire(f);
                        portfolio_.on_fill(f);
                        strategy_->set_position_open(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                        publish_event(fill_ptr);
                        if (!config_.is_threaded())
                            analytics_.on_event(fill_ptr);
                    }
                }
            }
            break;
        }
        case event_type::tick: {
            auto& te = static_cast<tick_event&>(*ev);
            last_mid_price_ = te.get_price();
            publish_event(ev);
            if (!config_.is_threaded())
                analytics_.on_event(ev);

            auto order_opt = strategy_->on_tick(te);
            if (order_opt)
            {
                order_opt->set_order_id(OrderIdGenerator::next());
                order_opt->set_earliest_eligible_ts(te.get_timestamp());
                auto adapter = get_adapter(te.get_symbol());
                if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
                    local->set_mid_price(last_mid_price_);
                adapter->submit_order(*order_opt);

                auto order_ptr = order_pool_.acquire(*order_opt);
                publish_event(order_ptr);
                if (!config_.is_threaded())
                    analytics_.on_event(order_ptr);

                std::vector<fill_event> fills;
                if (adapter->poll_fills(fills))
                {
                    for (auto& f : fills)
                    {
                        auto fill_ptr = fill_pool_.acquire(f);
                        portfolio_.on_fill(f);
                        strategy_->set_position_open(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                        publish_event(fill_ptr);
                        if (!config_.is_threaded())
                            analytics_.on_event(fill_ptr);
                    }
                }
            }
            break;
        }
        default:
            // Other event types (signal, l2, etc.) just pass through analytics
            publish_event(ev);
            if (!config_.is_threaded())
                analytics_.on_event(ev);
            break;
        }

        event_count++;
    }

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << std::endl;
    std::cout << "Replay complete: " << event_count << " events in "
              << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

    stop_workers();
}
