#include "engine.h"
#include "checkpoint.h"
#ifdef HAS_QUESTDB
#include "data/questdb/run_tag.h"
#endif
#include "data/data_handler.h"
#include "data/date_parse.h"
#include "execution/portfolio.h"
#include "execution/latency_model.h"
#include "execution/execution_bridge.h"
#include "execution/trade_tape_shadow_adapter.h"
#include "execution/fill_parser.h"
#include "execution/queue_aware_book_adapter.h"
#include "execution/queue_model.h"
#include "providers/provider.h"
#include "providers/provider_convert.h"
#include "ui/console_dashboard.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>
#include <vector>
#include <queue>
#include <chrono>
#include <iomanip>

// ============================================================
// LIVE-SAFETY SURFACE — Phase 1 freeze (see prod.md)
// Any edit requires explicit two-person CCB review + 4 h
// mainnet shadow run on engine_shadow before merge.
// Files in this set: tt_target.h, engine.{h,cpp}, all
// *kill_switch*, *dead_mans_switch*, *reconciler* under
// providers/binance/, risk/*, ExecutionBridge, live_safety.h
// ============================================================

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
    market_maker_.set_calibration({config_.mm_levels_per_side,
                                   config_.mm_base_depth,
                                   config_.mm_base_spread_pct,
                                   config_.mm_vol_spread_mult,
                                   config_.mm_max_half_spread_pct});

    if (ob)
        orderbook_registry_ = OrderbookRegistry();


    if (config_.mode == engine_mode::shadow)
        shadow_tracker_ = std::make_unique<ShadowTracker>();

    if (config_.mode == engine_mode::shadow)
    {
        exchange_portfolio_.emplace(config_.initial_balance);
        exchange_analytics_.emplace(
            config_.initial_balance,
            config_.rolling_window,
            config_.risk_free_rate,
            config_.periods_per_year,
            config_.max_equity_points);
    }

    // Wire venue-bracket adapter if the provider offers one. Live mode
    // is the only setting where this currently kicks in (Binance OCO),
    // but the wiring is provider-driven, not mode-driven, so any future
    // adapter (e.g. shadow-mode replay of venue brackets) plugs in here.
    if (config_.provider)
    {
        if (auto rc = config_.provider->get_risk_check())
            risk_check_ = std::move(rc);

        // Phase 2: allow providers to publish custom events (funding_event, future liquidation_opportunity, etc.)
        // back into the engine's ring so they reach QuestDbWorker, analytics, risk workers, TUI, etc.
        config_.provider->set_event_publisher(
            [this](std::shared_ptr<event> ev) { publish_event(ev); });

        config_.provider->set_funding_event_factory(
            [this](std::chrono::system_clock::time_point ts,
                   const std::string& symbol,
                   double cash_delta,
                   const std::string& reason) {
                return acquire_pooled(funding_pool_, ts, symbol, 0.0,
                                      cash_delta, reason);
            });

        // Register any liveness sources the provider exposes. Only
        // create the watchdog if there's something to watch — engine
        // shouldn't pay for a poll thread it doesn't need.
        auto sources = config_.provider->get_liveness_sources();
        if (!sources.empty())
        {
            worker_watchdog_ = std::make_unique<WorkerWatchdog>();
            for (auto& s : sources)
            {
                if (s.last_alive_ms && s.deadline_ms > 0)
                    worker_watchdog_->register_source(
                        std::move(s.name), s.last_alive_ms, s.deadline_ms);
            }
            worker_watchdog_->set_halt_callback(
                [this](std::string_view source, std::int64_t age_ms) {
                    char msg[128];
                    std::snprintf(msg, sizeof(msg),
                                  "watchdog: '%.*s' silent for %lldms",
                                  static_cast<int>(source.size()), source.data(),
                                  static_cast<long long>(age_ms));
                    trigger_halt(msg);
                });
            worker_watchdog_->set_halt_flag(halt_flag_);
            worker_watchdog_->start();
        }

        if (auto ba = config_.provider->get_bracket_adapter())
            exit_manager_.set_bracket_adapter(std::move(ba));

        // Install the bridge-side hook that turns inbound fills for
        // venue-bracket legs (which never went through route_order, so
        // by_client_id_ misses them) into engine-recognized fill_events.
        if (auto adapter = config_.provider->get_execution_adapter())
        {
            if (auto* bridge = dynamic_cast<ExecutionBridge*>(adapter.get()))
            {
                bridge->set_unknown_fill_handler(
                    [this](const parsed_exec& msg, std::uint64_t fill_id)
                        -> std::optional<ExecutionBridge::synth_result>
                    {
                        const auto opener = exit_manager_
                            .opener_for_exchange_order(msg.exchange_order_id);
                        if (opener == 0) return std::nullopt;

                        auto strategy_name = exit_manager_
                            .strategy_name_for_exchange_order(msg.exchange_order_id);

                        const auto engine_id = OrderIdGenerator::next();
                        const auto ts = (msg.ts.time_since_epoch().count() != 0)
                            ? msg.ts : std::chrono::system_clock::now();
                        const double remaining =
                            (msg.k == parsed_exec::kind::partial_fill)
                              ? std::max(0.0, msg.cumulative_qty - msg.last_fill_qty)
                              : 0.0;

                        fill_event fe(ts, msg.symbol, engine_id, msg.side,
                                      msg.last_fill_qty, msg.last_fill_price,
                                      msg.commission, remaining, fill_id);
                        fe.set_source(fill_source::exchange);
                        if (!strategy_name.empty()) fe.set_strategy_name(strategy_name);
                        if (opener != 0) fe.set_opener_order_id(opener);
                        return ExecutionBridge::synth_result{
                            std::move(fe), opener, std::move(strategy_name)};
                    });
            }
        }
    }

    restore_from_checkpoint();

    if (config_.mode == engine_mode::live)
    {
        // Route a fatal transport disconnect (WS idle timeout, listenKey
        // failure beyond retry budget) straight into trigger_halt so the
        // engine begins shutdown within ~2.5s of the loss instead of
        // burning minutes in the transport-level reconnect loop. Backtest
        // and shadow paths leave halt_callback unset; their providers
        // keep the original reconnect-and-continue behaviour.
        if (config_.provider)
        {
            config_.provider->set_halt_callback(
                [this](std::string_view reason) {
                    trigger_halt(reason);
                });
        }

        auto reconciler = config_.reconciler;
        if (!reconciler && config_.provider)
            reconciler = config_.provider->get_reconciler();
        if (!reconciler)
            reconciler = std::make_shared<NoopReconciler>();

        auto err = reconciler->reconcile(portfolio_, config_.reconcile_tolerance_bps);
        if (!err.empty())
            throw std::runtime_error("reconciliation refused startup: " + err);

        // Restart safety: rehydrate any venue-resting brackets from a
        // previous run so the watchdog evaluates them and our cancel
        // paths know about them. Without this, an engine restart would
        // leave orphan OCO orders on the venue and the in-process
        // ExitManager would have no record of them.
        if (auto adapter = exit_manager_.has_bracket_adapter()
                                ? config_.provider->get_bracket_adapter()
                                : nullptr)
        {
            for (auto& rb : adapter->list_open())
            {
                std::cerr << "engine: rehydrating bracket opener="
                          << rb.opener_order_id << " symbol=" << rb.symbol
                          << " sl=" << (rb.stop_loss   ? *rb.stop_loss   : 0.0)
                          << " tp=" << (rb.take_profit ? *rb.take_profit : 0.0)
                          << "\n";
                exit_manager_.rehydrate(rb);
            }
        }
    }

    prewarm_object_pools();
}

void engine::prewarm_object_pools()
{
    const auto& pw = config_.pool_prewarm;

    auto setup = [&](const char* name, std::size_t min_blocks, auto& pool) {
        pool.set_pool_name(name);
        pool.ensure_min_blocks(min_blocks);
        pool.set_forbid_runtime_grow(pw.forbid_runtime_grow);
    };

    setup("market_pool",      pw.market_blocks,      market_pool_);
    setup("tick_pool",        pw.tick_blocks,        tick_pool_);
    setup("order_pool",       pw.order_blocks,       order_pool_);
    setup("fill_pool",        pw.fill_blocks,        fill_pool_);
    setup("l2_update_pool",   pw.l2_update_blocks,   l2_update_pool_);
    setup("l2_snapshot_pool", pw.l2_snapshot_blocks, l2_snapshot_pool_);
    setup("rejection_pool",   pw.rejection_blocks,   rejection_pool_);
    setup("cancel_pool",      pw.cancel_blocks,      cancel_pool_);
    setup("amend_pool",       pw.amend_blocks,       amend_pool_);
    setup("funding_pool",     pw.funding_blocks,     funding_pool_);

    std::size_t total_event_slots =
        market_pool_.capacity_slots() + tick_pool_.capacity_slots()
        + order_pool_.capacity_slots() + fill_pool_.capacity_slots()
        + l2_update_pool_.capacity_slots() + l2_snapshot_pool_.capacity_slots()
        + rejection_pool_.capacity_slots() + cancel_pool_.capacity_slots()
        + amend_pool_.capacity_slots() + funding_pool_.capacity_slots()
        ;

    std::size_t cb_slots = pw.control_block_slots;
    if (cb_slots == 0)
        cb_slots = total_event_slots;

    const std::size_t cb_blocks =
        (cb_slots + ControlBlockPool::slots_per_block() - 1)
        / ControlBlockPool::slots_per_block();

    control_block_pool_.set_pool_name("control_block_pool");
    control_block_pool_.ensure_min_blocks(std::max(cb_blocks, std::size_t{1}));
    control_block_pool_.set_forbid_runtime_grow(pw.forbid_runtime_grow);

    auto wire_cb = [&](auto& pool) {
        pool.set_control_block_pool(&control_block_pool_);
    };
    wire_cb(market_pool_);
    wire_cb(tick_pool_);
    wire_cb(order_pool_);
    wire_cb(fill_pool_);
    wire_cb(l2_update_pool_);
    wire_cb(l2_snapshot_pool_);
    wire_cb(rejection_pool_);
    wire_cb(cancel_pool_);
    wire_cb(amend_pool_);
    wire_cb(funding_pool_);

    // Orderbook orders: pool bodies only; CBs stay on heap (lambda deleter
    // + order type exceed the 64-byte CB slot on some libstdc++ builds).
    orderbook_registry_.set_order_pool_config(nullptr,
                                              pw.orderbook_order_blocks,
                                              pw.forbid_runtime_grow);
}

void engine::drain_object_pool_returns() noexcept
{
    market_pool_.drain_deferred_returns();
    order_pool_.drain_deferred_returns();
    fill_pool_.drain_deferred_returns();
    tick_pool_.drain_deferred_returns();
    l2_update_pool_.drain_deferred_returns();
    l2_snapshot_pool_.drain_deferred_returns();
    rejection_pool_.drain_deferred_returns();
    cancel_pool_.drain_deferred_returns();
    amend_pool_.drain_deferred_returns();
    funding_pool_.drain_deferred_returns();
    control_block_pool_.drain_deferred_returns();
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
    data_handler_->db_data_symbol.clear();
    data_handler_->db_data_symbol.push_back(new_symbol);

    strategy_->set_position_open(new_symbol, false);
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

        if (config_.maker_queue_model)
        {
            // Use queue-aware paper execution for passive limits
            auto qa = std::make_shared<QueueAwareBookAdapter>(
                config_.maker_queue_model,
                config_.fee_model,
                config_.latency_model);
            adapter = qa;
        }
        else
        {
            auto local = std::make_shared<LocalBookAdapter>(
                ob, config_.fee_model, config_.fill_model,
                config_.seed != 0 ? static_cast<unsigned>(config_.seed + 2) : config_.fill_rng_seed,
                config_.market_aggression, config_.qty_scale,
                config_.latency_model, config_.impact_model,
                config_.walked_book_impact);
            if (config_.debug_fills)
                local->set_debug_fills(true, config_.debug_fills_budget);
            adapter = local;
        }
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
    refresh_dashboard_view_if_due();

    // Phase 2: funding settlements update the primary portfolio cash immediately
    // (advisory for now; later will also feed risk_snapshot / RiskManager).
    if (ev && ev->get_type() == event_type::funding) {
        if (auto* fe = dynamic_cast<funding_event*>(ev.get())) {
            portfolio_.on_funding(*fe);
#ifdef HAS_QUESTDB
            if (questdb_store_) {
                questdb_store_->record_funding(*fe, questdb_store_->run_tag());
            }
#endif
        }
    }

    if (config_.threading == thread_preset::inline_mode) {
        return;
    }

    // `safety` flags rings whose drops would leave the engine trading past
    // a risk limit; halt_on_drop escalates a safety drop into a full halt.
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
            if (config_.drop_policy == ring_drop_policy::block)
            {
                // Deterministic backpressure: never lose an event — wait for
                // the worker to drain the ring. Bail out (and count a drop)
                // only when blocking can no longer terminate: a worker died
                // or the engine is halting.
                for (;;)
                {
                    if (worker_failed_.load(std::memory_order_acquire) ||
                        halt_flag_.load(std::memory_order_acquire))
                        break;
                    std::this_thread::yield();
                    if (ring->try_push(ev))
                    {
#ifdef HAS_DEBUG
                        diag.on_push(ring->occupancy());
#endif
                        return;
                    }
                }
            }
            ++drops;
#ifdef HAS_DEBUG
            diag.on_drop();
#endif
            if (auto* dash = config_.dashboard.get())
            {
                auto& st = dash->stats();
                if      (std::string_view(name) == "logging")    st.ring_drops_logging.store(drops, std::memory_order_relaxed);
                else if (std::string_view(name) == "risk")       st.ring_drops_risk.store(drops, std::memory_order_relaxed);
                else if (std::string_view(name) == "stats")      st.ring_drops_stats.store(drops, std::memory_order_relaxed);
                else if (std::string_view(name) == "observer")   st.ring_drops_observer.store(drops, std::memory_order_relaxed);
                else if (std::string_view(name) == "risk_stats") st.ring_drops_risk_stats.store(drops, std::memory_order_relaxed);
                else if (std::string_view(name) == "mm")         st.ring_drops_mm.store(drops, std::memory_order_relaxed);
            }
            if (safety && config_.drop_policy == ring_drop_policy::halt_on_drop)
            {
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "ring drop on '%s' (%zu dropped) — halted",
                              name, drops);
                trigger_halt(msg);
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
}

void engine::trigger_halt(std::string_view reason)
{
    if (halt_flag_.exchange(true, std::memory_order_acq_rel))
        return;

    if (auto* dash = config_.dashboard.get())
    {
        dash->stats().halt_flag.store(true, std::memory_order_release);
        dash->set_state(truetest::ui::connection_state::halted);
        dash->set_shutdown_reason(reason);
        dash->push_event(truetest::ui::event_severity::error, reason);
    }
    else
    {
        std::cerr << "  ! engine halt — " << reason << "\n";
    }
}

bool engine::snapshot_dashboard(truetest::ui::dashboard_snapshot& out) const
{
    std::lock_guard<std::mutex> lk(dashboard_view_mu_);
    if (!dashboard_view_initialised_) return false;
    out = dashboard_view_;
    return true;
}

void engine::request_dashboard_refresh()
{
    // Force the next refresh to happen immediately, bypassing the normal debounce.
    // This is used by the rich TUI on tab switches, unfreeze, pause, etc. to
    // give the operator fresh data right away (Fix #3 → Fixed).
    dashboard_view_force_ = true;
    dashboard_view_last_ = std::chrono::steady_clock::time_point{};
}

void engine::refresh_dashboard_view_if_due()
{
    auto now = std::chrono::steady_clock::now();

    // If a force was requested (e.g. from the rich TUI on tab switch / unfreeze),
    // always refresh this time.
    if (!dashboard_view_force_
        && dashboard_view_initialised_
        && now - dashboard_view_last_ < dashboard_view_interval_)
    {
        return;
    }

    truetest::ui::dashboard_snapshot snap;
    build_dashboard_view(snap);

    {
        std::lock_guard<std::mutex> lk(dashboard_view_mu_);
        dashboard_view_              = std::move(snap);
        dashboard_view_initialised_  = true;
    }
    dashboard_view_last_ = now;
    dashboard_view_force_ = false;   // consumed
}

void engine::cache_open_order(const order_event& o)
{
    open_order_cache_entry e;
    e.row.order_id      = o.get_order_id();
    e.row.symbol        = o.get_symbol();
    e.row.strategy_name = o.get_strategy_name();
    e.row.side          = (o.get_side() == order_side::buy) ? 'B' : 'S';
    switch (o.get_order_type())
    {
        case order_type::market:     e.row.type = 'M'; break;
        case order_type::limit:      e.row.type = 'L'; break;
        case order_type::stop:       e.row.type = 'S'; break;
        case order_type::stop_limit: e.row.type = 's'; break;
    }
    e.row.qty    = o.get_quantity();
    e.row.price  = o.get_price();
    e.row.status = "open";
    e.ts         = o.get_timestamp();
    open_orders_cache_[o.get_order_id()] = std::move(e);
}

void engine::update_open_order_status(std::uint64_t id, const char* status)
{
    auto it = open_orders_cache_.find(id);
    if (it != open_orders_cache_.end())
        it->second.row.status = status;
}

void engine::erase_open_order(std::uint64_t id)
{
    open_orders_cache_.erase(id);
}

void engine::cache_fill(const fill_event& f)
{
    truetest::ui::dashboard_snapshot::fill_row r;
    r.ts     = f.get_timestamp();
    r.symbol = f.get_symbol();
    r.side   = (f.get_side() == order_side::buy) ? 'B' : 'S';
    r.qty    = f.get_filled_quantity();
    r.price  = f.get_fill_price();
    r.fee    = f.get_commission();
    r.source = (f.get_source() == fill_source::exchange)  ? "exchange"
             : (f.get_source() == fill_source::simulated) ? "simulated"
             :                                              "local";
    recent_fills_cache_.push_front(std::move(r));
    while (recent_fills_cache_.size() > kRecentFillsCap)
        recent_fills_cache_.pop_back();
}

void engine::build_dashboard_view(truetest::ui::dashboard_snapshot& out) const
{
    using snap_t = truetest::ui::dashboard_snapshot;

    // Account
    out.cash             = portfolio_.get_cash();
    out.initial_balance  = portfolio_.get_initial_balance();
    out.equity           = portfolio_.get_equity(last_mid_price_);
    out.realized_pnl     = out.equity - out.initial_balance;   // approximation
    out.unrealized_pnl   = 0.0;

    // Positions
    out.positions.clear();
    out.positions.reserve(portfolio_.get_positions().size());
    for (const auto& [sym, pos] : portfolio_.get_positions())
    {
        if (std::abs(pos.qty) < 1e-12) continue;
        snap_t::position_row row;
        row.symbol     = sym;
        row.qty        = pos.qty;
        row.avg_entry  = (std::abs(pos.qty) > 0.0)
                           ? pos.cost_basis / pos.qty : 0.0;
        row.mark       = (sym == last_mark_symbol_) ? last_mid_price_ : 0.0;
        row.unrealized = (row.mark > 0.0)
                           ? (row.mark - row.avg_entry) * pos.qty
                           : 0.0;
        out.unrealized_pnl += row.unrealized;
        out.positions.push_back(std::move(row));
    }

    // Lots — per-strategy attribution alongside the netted positions.
    out.lots.clear();
    out.lots.reserve(portfolio_.get_lots().size());
    auto now_sys = std::chrono::system_clock::now();
    for (const auto& [opener_id, l] : portfolio_.get_lots())
    {
        snap_t::lot_row row;
        row.opener_order_id = opener_id;
        row.symbol          = l.symbol;
        row.strategy_name   = l.strategy_name;
        row.side            = (l.side == order_side::buy) ? 'L' : 'S';
        row.qty_open        = l.qty_open;
        row.entry_price     = l.entry_price;
        row.age_seconds     = std::chrono::duration_cast<std::chrono::seconds>(
                                  now_sys - l.ts_open).count();
        out.lots.push_back(std::move(row));
    }

    // Open orders — copy from cache, computing age vs now.
    out.open_orders.clear();
    out.open_orders.reserve(open_orders_cache_.size());
    auto now_steady_sys = std::chrono::system_clock::now();
    for (const auto& [id, e] : open_orders_cache_)
    {
        auto row = e.row;
        row.age_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              now_steady_sys - e.ts).count();
        out.open_orders.push_back(std::move(row));
    }

    // Recent fills — newest first.
    out.recent_fills.assign(recent_fills_cache_.begin(),
                            recent_fills_cache_.end());

    // Markout from adverse-selection tracker (post-fill mid drift).
    out.perf.avg_markout_bps = adverse_selection_.mean_bps();
    out.perf.markout_samples = adverse_selection_.sample_count();
    out.perf.total_fills     = portfolio_.get_total_fills();
    out.perf.total_trades    = portfolio_.get_total_trades();
    out.perf.total_orders    = open_orders_cache_.size()   // active right now
                              + portfolio_.get_total_fills(); // historical lower bound
    out.perf.win_rate        = analytics_.win_rate_pct();
    out.perf.sharpe          = analytics_.rolling_sharpe();
    out.perf.sortino         = 0.0;             // requires full report — skip
    out.perf.profit_factor   = 0.0;             // requires full report — skip

    // Risk view.
    out.risk.halted             = halt_flag_.load(std::memory_order_acquire);
    out.risk.daily_loss         = 0.0;          // RiskManager doesn't expose
    out.risk.daily_loss_limit   = config_.risk.max_daily_loss;
    out.risk.max_drawdown_pct   = analytics_.max_drawdown_pct();
    out.risk.max_drawdown_limit = config_.risk.max_drawdown * 100.0;
    out.risk.open_orders        = open_orders_cache_.size();
    out.risk.open_orders_limit  =
        (config_.risk.max_open_orders > 0)
            ? static_cast<std::size_t>(config_.risk.max_open_orders)
            : 0;

    // Exposure: |qty| * mark per symbol, summed.
    double exposure = 0.0;
    for (const auto& [sym, pos] : portfolio_.get_positions())
    {
        if (std::abs(pos.qty) < 1e-12) continue;
        double mark = (sym == last_mark_symbol_) ? last_mid_price_ : 0.0;
        if (mark > 0.0) exposure += std::abs(pos.qty) * mark;
    }
    out.risk.exposure       = exposure;
    out.risk.exposure_limit = config_.risk.max_portfolio_exposure;

    // Queue (maker paper + shadow L2 model). Mirrors the logic in
    // write_adapter_diagnostics + print_summary but for the snapshot
    // (TUI "debug" / future queue panel). Cheap; called at ~100ms debounce.
    // Phase 2: now also wires the richer TradeTapeShadowAdapter stats
    // (submitted_with_queue, filled_after_drain, blocked_at_eos) for
    // divergence analysis and UI.
    {
        std::uint64_t qsum = 0;
        std::uint32_t qn = 0;
        std::size_t submitted = 0, filled = 0, blocked = 0;
        auto collect = [&](IExecutionAdapter* a) {
            if (!a) return;
            const auto c = a->live_quote_count();
            if (c == 0) return;
            qsum += a->avg_queue_position_bps();
            ++qn;
            submitted += a->queue_submitted_with_queue();
            filled    += a->queue_filled_after_drain();
            blocked   += a->queue_blocked_at_eos();
        };
        for (auto& [_, ad] : execution_adapters_)
            collect(ad.get());
        if (config_.provider)
            collect(config_.provider->get_execution_adapter().get());
        out.queue.avg_bps = (qn > 0) ? static_cast<std::uint32_t>(qsum / qn) : 0u;
        out.queue.submitted_with_queue = submitted;
        out.queue.filled_after_drain   = filled;
        out.queue.blocked_at_eos       = blocked;
    }

    // Brackets — armed bracket intents from ExitManager. Each row is
    // matched to its symbol's mark from the positions table so the
    // panel can render distance-to-trigger directly.
    out.brackets.clear();
    auto armed = exit_manager_.snapshot_armed();
    out.brackets.reserve(armed.size());
    for (const auto& a : armed)
    {
        snap_t::bracket_row row;
        row.opener_order_id = a.opener_order_id;
        row.strategy_name   = a.strategy_name;
        row.symbol          = a.symbol;
        row.side            = (a.close_side == order_side::sell) ? 'L' : 'S';
        row.qty             = a.qty;
        row.entry_price     = a.entry_price;
        row.stop_loss       = a.stop_loss;
        row.take_profit     = a.take_profit;
        row.venue_managed   = a.venue_managed;
        row.venue_list_id   = a.venue_list_id;
        row.age_seconds     = std::chrono::duration_cast<std::chrono::seconds>(
                                  now_sys - a.ts_armed).count();
        // Mark from the per-symbol position row (already computed above).
        for (const auto& p : out.positions)
        {
            if (p.symbol == a.symbol) { row.mark = p.mark; break; }
        }
        if (row.mark == 0.0 && a.symbol == last_mark_symbol_)
            row.mark = last_mid_price_;
        out.brackets.push_back(std::move(row));
    }

    // L2 ladder for the active symbol. Reads the orderbook directly;
    // the only allocation is the small bid/ask vectors. Source flag
    // disambiguates real venue depth (provider's L2 stream tagged the
    // symbol in l2_seeded_symbols_) from MM-seeded synthetic depth.
    {
        using snap_t = truetest::ui::dashboard_snapshot;
        auto& v = out.l2;
        v = snap_t::l2_view{};

        constexpr std::size_t kDepthRows = 10;
        const std::string& sym = last_mark_symbol_;
        v.symbol = sym;

        if (!sym.empty())
        {
            auto ob = orderbook_registry_.get(sym);
            if (ob)
            {
                auto infos = ob->get_order_infos();
                const auto& bids = infos.get_bids();   // best-first (high → low)
                const auto& asks = infos.get_asks();   // best-first (low  → high)
                v.total_bid_levels = bids.size();
                v.total_ask_levels = asks.size();

                // Truncate to top-N + walk cumulative. Both vectors are
                // already sorted best-first by the orderbook itself.
                double cb = 0.0;
                v.bids.reserve(std::min(kDepthRows, bids.size()));
                for (std::size_t i = 0; i < std::min(kDepthRows, bids.size()); ++i)
                {
                    snap_t::l2_level l;
                    l.price = bids[i].price_.to_double();
                    l.size  = static_cast<double>(bids[i].quantity_) / config_.qty_scale;
                    cb += l.size;
                    l.cum   = cb;
                    v.bids.push_back(l);
                }
                v.cum_bid_size = cb;

                double ca = 0.0;
                v.asks.reserve(std::min(kDepthRows, asks.size()));
                for (std::size_t i = 0; i < std::min(kDepthRows, asks.size()); ++i)
                {
                    snap_t::l2_level l;
                    l.price = asks[i].price_.to_double();
                    l.size  = static_cast<double>(asks[i].quantity_) / config_.qty_scale;
                    ca += l.size;
                    l.cum   = ca;
                    v.asks.push_back(l);
                }
                v.cum_ask_size = ca;

                if (!v.bids.empty()) v.best_bid = v.bids.front().price;
                if (!v.asks.empty()) v.best_ask = v.asks.front().price;
                if (v.best_bid > 0.0 && v.best_ask > 0.0)
                {
                    v.mid = (v.best_bid + v.best_ask) * 0.5;
                    v.spread_bps = (v.best_ask - v.best_bid) / v.mid * 1e4;
                    // Microprice — size-weighted mid that leans toward
                    // the side with more depth at the BBO.
                    const double bsz = v.bids.front().size;
                    const double asz = v.asks.front().size;
                    const double tot = bsz + asz;
                    v.microprice = (tot > 0.0)
                        ? (bsz * v.best_ask + asz * v.best_bid) / tot
                        : v.mid;
                }
                if (v.cum_bid_size + v.cum_ask_size > 0.0)
                    v.imbalance = (v.cum_bid_size - v.cum_ask_size)
                                / (v.cum_bid_size + v.cum_ask_size);

                // Source: venue if L2 stream tagged the symbol; else
                // synthetic (MM seeded) if any depth exists; else none.
                if (l2_seeded_symbols_.count(sym))
                    v.source = snap_t::l2_source::venue;
                else if (!v.bids.empty() || !v.asks.empty())
                    v.source = snap_t::l2_source::synthetic;
                else
                    v.source = snap_t::l2_source::none;
            }
        }
    }

    // Memory view — RSS/heap from /proc (Linux only) + computed pool /
    // ring footprints. Cached at ~1 Hz: memory doesn't move in 100 ms
    // and /proc parsing was the dominant cost of the whole snapshot
    // path. The cache is mutable so this const method can update it.
    {
        const auto now_steady = std::chrono::steady_clock::now();
        const bool stale = !memory_cache_initialised_
            || (now_steady - memory_cache_last_) >= std::chrono::seconds(1);

        if (stale)
        {
            auto& m = memory_cache_;
            m = truetest::ui::dashboard_snapshot::memory_view{};

            // Single-pass /proc/self/status parse. Earlier code opened
            // the file three times (once per key); now one read scans
            // for all three. Saves ~10 KB of redundant I/O per refresh.
            std::ifstream sf("/proc/self/status");
            if (sf.is_open())
            {
                std::string line;
                int hits = 0;
                while (hits < 3 && std::getline(sf, line))
                {
                    auto extract_kib = [&](const char* key) -> std::uint64_t {
                        const std::size_t klen = std::strlen(key);
                        if (line.compare(0, klen, key) != 0) return 0;
                        std::istringstream ss(line);
                        std::string k;
                        std::uint64_t v = 0;
                        ss >> k >> v;
                        return v * 1024;
                    };
                    if (auto v = extract_kib("VmRSS:"))  { m.rss_bytes      = v; ++hits; continue; }
                    if (auto v = extract_kib("VmSize:")) { m.vm_bytes       = v; ++hits; continue; }
                    if (auto v = extract_kib("VmHWM:"))  { m.peak_rss_bytes = v; ++hits; continue; }
                }
            }

            // /proc/self/statm — column 6 is "data" (heap+BSS+stack) in pages.
            {
                std::ifstream f("/proc/self/statm");
                if (f.is_open())
                {
                    std::uint64_t size, resident, shared, text, lib, data, dt;
                    if (f >> size >> resident >> shared >> text >> lib >> data >> dt)
                        m.heap_bytes = data * 4096;
                }
            }
            m.available = (m.rss_bytes > 0);

            // /proc/self/maps — categorise mapped regions so the panel
            // can break down the "other" segment into heap / stacks /
            // shared-lib code / anonymous mmap. Each line:
            //   <start>-<end> <perms> <off> <dev> <ino> <path>
            // sizes are summed in BYTES, not pages, since the address
            // range itself is the size.
            {
                std::ifstream f("/proc/self/maps");
                std::uint64_t b_heap = 0, b_stacks = 0, b_so = 0,
                              b_anon = 0, b_file = 0;
                if (f.is_open())
                {
                    std::string line;
                    while (std::getline(f, line))
                    {
                        // Parse the address range up to the dash.
                        const auto dash = line.find('-');
                        if (dash == std::string::npos) continue;
                        const auto sp1  = line.find(' ', dash + 1);
                        if (sp1 == std::string::npos) continue;
                        std::uint64_t a = 0, b = 0;
                        try {
                            a = std::stoull(line.substr(0, dash), nullptr, 16);
                            b = std::stoull(line.substr(dash + 1, sp1 - dash - 1),
                                            nullptr, 16);
                        } catch (...) { continue; }
                        if (b <= a) continue;
                        const std::uint64_t bytes = b - a;

                        // Path is everything after the last whitespace
                        // run (or empty for anonymous mappings).
                        const auto path_pos = line.find_last_of(' ');
                        std::string path = (path_pos != std::string::npos)
                            ? line.substr(path_pos + 1) : std::string{};

                        if (path == "[heap]")              b_heap   += bytes;
                        else if (path.rfind("[stack", 0) == 0)
                                                            b_stacks += bytes;
                        else if (path.size() >= 3 &&
                                 path.find(".so") != std::string::npos)
                                                            b_so     += bytes;
                        else if (path.empty() || path[0] == '[')
                                                            b_anon   += bytes;
                        else                                b_file   += bytes;
                    }
                }
                using snap_t = truetest::ui::dashboard_snapshot;
                if (b_heap   > 0) m.other_breakdown.push_back({"heap",      b_heap});
                if (b_stacks > 0) m.other_breakdown.push_back({"stacks",    b_stacks});
                if (b_so     > 0) m.other_breakdown.push_back({".so libs",  b_so});
                if (b_file   > 0) m.other_breakdown.push_back({"file-mmap", b_file});
                if (b_anon   > 0) m.other_breakdown.push_back({"anon-mmap", b_anon});
            }

            // Pools: ObjectPool::block_count() is now lock-free atomic
            // (see #3 in this commit), so this is just an atomic load
            // per pool. Cached anyway since stale-check already gates us.
            constexpr std::size_t kPoolBlock = 4096;
            auto add_pool = [&](const char* name, std::size_t blocks,
                                std::size_t slot, std::size_t in_use,
                                std::size_t grows) {
                using snap_t = truetest::ui::dashboard_snapshot;
                snap_t::mem_pool_row r;
                r.name           = name;
                r.blocks         = blocks;
                r.slot_size      = slot;
                r.bytes          = static_cast<std::uint64_t>(blocks) * kPoolBlock * slot;
                r.in_use         = in_use;
                r.capacity_slots = blocks * kPoolBlock;
                r.grow_count     = grows;
                m.pool_bytes_total += r.bytes;
                m.pools.push_back(r);
            };
            add_pool("market_pool",      market_pool_.block_count(),
                     sizeof(market_event), market_pool_.in_use(), market_pool_.grow_count());
            add_pool("order_pool",       order_pool_.block_count(),
                     sizeof(order_event),  order_pool_.in_use(),  order_pool_.grow_count());
            add_pool("fill_pool",        fill_pool_.block_count(),
                     sizeof(fill_event),   fill_pool_.in_use(),   fill_pool_.grow_count());
            add_pool("tick_pool",        tick_pool_.block_count(),
                     sizeof(tick_event),   tick_pool_.in_use(),   tick_pool_.grow_count());
            add_pool("l2_update_pool",   l2_update_pool_.block_count(),
                     sizeof(l2_update_event), l2_update_pool_.in_use(), l2_update_pool_.grow_count());
            add_pool("l2_snapshot_pool", l2_snapshot_pool_.block_count(),
                     sizeof(l2_snapshot_event), l2_snapshot_pool_.in_use(), l2_snapshot_pool_.grow_count());
            add_pool("rejection_pool",   rejection_pool_.block_count(),
                     sizeof(rejection_event), rejection_pool_.in_use(), rejection_pool_.grow_count());
            add_pool("cancel_pool",      cancel_pool_.block_count(),
                     sizeof(cancel_event), cancel_pool_.in_use(), cancel_pool_.grow_count());
            add_pool("amend_pool",       amend_pool_.block_count(),
                     sizeof(amend_event), amend_pool_.in_use(), amend_pool_.grow_count());
            add_pool("funding_pool",     funding_pool_.block_count(),
                     sizeof(funding_event), funding_pool_.in_use(), funding_pool_.grow_count());
            add_pool("control_block_pool", control_block_pool_.block_count(),
                     ControlBlockPool::slot_size(), control_block_pool_.in_use(),
                     control_block_pool_.grow_count());

            // Rings: capacity is constexpr; this is essentially free.
            auto add_ring = [&](const char* name, const auto& ring) {
                using snap_t = truetest::ui::dashboard_snapshot;
                snap_t::mem_ring_row r;
                r.name = name;
                if (ring) {
                    r.capacity      = ring->capacity();
                    r.element_bytes = sizeof(event_pointer);
                    r.bytes = static_cast<std::uint64_t>(r.capacity) * r.element_bytes;
                    m.ring_bytes_total += r.bytes;
                }
                m.rings.push_back(r);
            };
            add_ring("logging",    logging_ring_);
            add_ring("risk",       risk_ring_);
            add_ring("stats",      stats_ring_);
            add_ring("observer",   observer_ring_);
            add_ring("risk_stats", risk_stats_ring_);
            add_ring("mm_event",   mm_ring_);
            add_ring("mm_order",   mm_order_ring_);

            memory_cache_last_ = now_steady;
            memory_cache_initialised_ = true;
        }
        out.memory = memory_cache_;

        // In-use counts refresh every snapshot — atomic loads are free,
        // and the panel's pool fill-bars look frozen if they lag the
        // cached interval. Order matches the add_pool() calls above.
        if (out.memory.pools.size() == 4)
        {
            out.memory.pools[0].in_use = market_pool_.in_use();
            out.memory.pools[1].in_use = order_pool_.in_use();
            out.memory.pools[2].in_use = fill_pool_.in_use();
            out.memory.pools[3].in_use = tick_pool_.in_use();
        }
    }

    // Debug view — engine introspection for the Debug tab. All sourced
    // from existing accessors / atomics; no new hot-path tracking. Built
    // once per snapshot under the same lock as everything else.
    {
        auto& d = out.debug;
        // engine_core is built without TT_TARGET (per-binary define), so
        // we surface the runtime mode here instead of the binary name.
        // The status bar already shows which binary you're in via the
        // window chrome / process name.
        d.target = "(engine_core)";
        switch (config_.mode) {
            case engine_mode::backtest: d.mode = "backtest"; break;
            case engine_mode::shadow:   d.mode = "shadow";   break;
            case engine_mode::live:     d.mode = "live";     break;
        }
#ifdef HAS_BINANCE
        d.has_binance = true;
#endif
#ifdef HAS_QUESTDB
        d.has_questdb = true;
#endif
#ifdef HAS_DEBUG
        d.has_debug = true;
#endif
#ifdef HAS_LIVE_DATA
        d.has_live_data = true;
#endif

        d.preset = preset_to_string(config_.threading);
        d.spin_policy = spin_policy_to_string(config_.worker_spin_policy);
        d.cpu_pin    = !config_.disable_pinning;

        // Worker count: derived from which ring members are non-null.
        std::size_t workers = 0;
        if (logging_ring_)    ++workers;
        if (risk_ring_)       ++workers;
        if (stats_ring_)      ++workers;
        if (observer_ring_)   ++workers;
        if (risk_stats_ring_) ++workers;
        if (mm_ring_ || mm_order_ring_) ++workers;
        d.worker_count = workers;

        // Ring stats — capacity 0 indicates the ring isn't running at
        // this preset (engine returns nullptr from the get_*_ring()).
        auto add_ring = [&](const char* name, const auto& ring) {
            using snap_t = truetest::ui::dashboard_snapshot;
            snap_t::ring_stat r;
            r.name = name;
            if (ring) {
                r.size = ring->size();
                r.hwm  = ring->high_watermark();
                r.capacity = ring->capacity();
                r.drops = ring->drop_count();
            }
            d.rings.push_back(r);
        };
        add_ring("logging",    logging_ring_);
        add_ring("risk",       risk_ring_);
        add_ring("stats",      stats_ring_);
        add_ring("observer",   observer_ring_);
        add_ring("risk_stats", risk_stats_ring_);
        add_ring("mm_event",   mm_ring_);
        // mm_order_ring_ uses MMRing (different element type) — track separately.
        {
            using snap_t = truetest::ui::dashboard_snapshot;
            snap_t::ring_stat r;
            r.name = "mm_order";
            if (mm_order_ring_) {
                r.size = mm_order_ring_->size();
                r.hwm  = mm_order_ring_->high_watermark();
                r.capacity = mm_order_ring_->capacity();
                r.drops = mm_order_ring_->drop_count();
            }
            d.rings.push_back(r);
        }

        constexpr std::size_t kPoolBlock = 4096;
        auto add_pool = [&](const char* name, std::size_t blocks,
                            std::size_t in_use, std::size_t grows) {
            using snap_t = truetest::ui::dashboard_snapshot;
            snap_t::pool_stat p;
            p.name = name;
            p.blocks = blocks;
            p.block_size = kPoolBlock;
            p.capacity = blocks * kPoolBlock;
            p.in_use = in_use;
            p.grow_count = grows;
            d.pools.push_back(p);
        };
        add_pool("market_pool",      market_pool_.block_count(),
                 market_pool_.in_use(), market_pool_.grow_count());
        add_pool("order_pool",       order_pool_.block_count(),
                 order_pool_.in_use(), order_pool_.grow_count());
        add_pool("fill_pool",        fill_pool_.block_count(),
                 fill_pool_.in_use(), fill_pool_.grow_count());
        add_pool("tick_pool",        tick_pool_.block_count(),
                 tick_pool_.in_use(), tick_pool_.grow_count());
        add_pool("l2_update_pool",   l2_update_pool_.block_count(),
                 l2_update_pool_.in_use(), l2_update_pool_.grow_count());
        add_pool("l2_snapshot_pool", l2_snapshot_pool_.block_count(),
                 l2_snapshot_pool_.in_use(), l2_snapshot_pool_.grow_count());
        add_pool("rejection_pool",   rejection_pool_.block_count(),
                 rejection_pool_.in_use(), rejection_pool_.grow_count());
        add_pool("cancel_pool",      cancel_pool_.block_count(),
                 cancel_pool_.in_use(), cancel_pool_.grow_count());
        add_pool("amend_pool",       amend_pool_.block_count(),
                 amend_pool_.in_use(), amend_pool_.grow_count());
        add_pool("funding_pool",     funding_pool_.block_count(),
                 funding_pool_.in_use(), funding_pool_.grow_count());
        add_pool("control_block_pool", control_block_pool_.block_count(),
                 control_block_pool_.in_use(), control_block_pool_.grow_count());

        // Engine state.
        d.next_order_id = OrderIdGenerator::next();
        // Decrement back so we don't perturb the live id sequence — the
        // call above only reads + increments atomically, so the value
        // we surface is "what the next allocation WOULD have been".
        // (We can't decrement an atomic safely if other threads bump it
        //  in between; subtract logically: the value we want is `next-1`
        //  treating it as the most-recently-allocated id, which is fine
        //  for an introspection display.)
        if (d.next_order_id > 0) --d.next_order_id;
        d.pending_orders    = pending_orders_.size();
        d.pending_stops     = pending_stops_.size();
        d.open_orders_cache = open_orders_cache_.size();
        d.order_meta_size   = order_meta_.size();
        d.armed_brackets    = exit_manager_.armed_count();
        d.handles_size      = exit_manager_.armed_count();   // proxy

        d.exit_pending = exit_manager_.pending_count();
        d.exit_armed   = exit_manager_.armed_count();
        d.exit_exchange_to_leg = 0;  // not exposed; left as 0

#ifdef HAS_DEBUG
        // Stage timings — only present on HAS_DEBUG builds. The stats
        // are accumulated counters, not percentile sketches; we surface
        // (calls / avg / min / max) which is what stage_stats actually
        // holds. Real percentiles would need an approximate-quantile
        // estimator wired into StageTimer::record().
        for (std::size_t i = 0; i < static_cast<std::size_t>(debug::stage::COUNT); ++i)
        {
            const auto st = static_cast<debug::stage>(i);
            const auto s  = stage_timer_.snapshot(st);
            if (s.call_count == 0) continue;
            using snap_t = truetest::ui::dashboard_snapshot;
            snap_t::debug_view::stage_row r;
            r.name   = debug::StageTimer::stage_name(st);
            r.calls  = s.call_count;
            r.avg_ns = s.total_ns / s.call_count;
            r.min_ns = s.min_ns;
            r.max_ns = s.max_ns;
            d.stages.push_back(r);
        }
#endif

        // Last errors — pull what's exposed; subsystems without a public
        // accessor get an empty string. Bridge has last_error() when the
        // provider supplies an ExecutionBridge adapter.
        using snap_t = truetest::ui::dashboard_snapshot;
        d.errors.push_back(snap_t::subsys_error{"engine",  ""});
        if (config_.provider)
        {
            auto adapter = config_.provider->get_execution_adapter();
            if (auto* bridge = dynamic_cast<ExecutionBridge*>(adapter.get()))
                d.errors.push_back(snap_t::subsys_error{"bridge", bridge->last_error()});
            else
                d.errors.push_back(snap_t::subsys_error{"bridge", ""});
        }
        else d.errors.push_back(snap_t::subsys_error{"bridge", ""});
        d.errors.push_back(snap_t::subsys_error{"provider", ""});
        d.errors.push_back(snap_t::subsys_error{"adapter",  ""});
        d.errors.push_back(snap_t::subsys_error{"questdb",  ""});
    }

    // Health view — latency from analytics + provider state. Ring drops,
    // event totals, and ages are read by the Health panel directly from
    // ConsoleDashboard's atomics (the engine does not own that ring).
    {
        auto lv = analytics_.latency_view_now();
        out.health.tick_to_trade_samples = lv.samples;
        out.health.avg_tick_to_trade_us  = lv.avg_ns / 1000.0;
        out.health.min_tick_to_trade_us  = static_cast<double>(lv.min_ns) / 1000.0;
        out.health.max_tick_to_trade_us  = static_cast<double>(lv.max_ns) / 1000.0;

        out.health.orders_total = open_orders_cache_.size()
                                + portfolio_.get_total_fills();
        out.health.fills_total  = portfolio_.get_total_fills();
        out.health.trades_total = portfolio_.get_total_trades();

        if (config_.provider)
        {
            out.health.provider_present = true;
            out.health.provider_name    = config_.provider->name();
            out.health.provider_state   = static_cast<int>(
                config_.provider->lifecycle_state());
        }

#ifdef HAS_QUESTDB
        if (questdb_active_ && questdb_store_)
        {
            auto qh = questdb_store_->health();
            out.health.questdb.active = true;
            out.health.questdb.connected = qh.connected;
            out.health.questdb.pending_lines = qh.pending_lines;
            out.health.questdb.dropped_lines = qh.dropped_lines;
            out.health.questdb.fallback_lines = qh.fallback_lines;
            out.health.questdb.strict_mode = qh.strict_mode;

            if (qh.last_flush.time_since_epoch().count() != 0)
            {
                auto age = std::chrono::steady_clock::now() - qh.last_flush;
                out.health.questdb.last_flush_age_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(age).count();
            }
        }
#endif
    }

    // Per-strategy attribution. analytics_.per_strategy_view() copies
    // the per-strategy map cheaply (O(K) for K strategies). Counts of
    // open lots and armed brackets are derived from the same passes
    // we already did over portfolio_/exit_manager_ above.
    out.strategies.clear();
    {
        // Tally lots and armed brackets per strategy in single passes.
        std::unordered_map<std::string, std::size_t> lots_by_strat;
        for (const auto& [_, l] : portfolio_.get_lots())
            ++lots_by_strat[l.strategy_name];
        std::unordered_map<std::string, std::size_t> brackets_by_strat;
        for (const auto& a : armed)
            ++brackets_by_strat[a.strategy_name];

        auto per_strat = analytics_.per_strategy_view();
        out.strategies.reserve(per_strat.size());
        for (const auto& [name, sa] : per_strat)
        {
            snap_t::strategy_row row;
            row.name           = name;
            row.pnl            = sa.total_pnl;
            row.trade_count    = sa.trade_count;
            row.win_count      = sa.win_count;
            row.win_rate       = sa.win_rate();
            row.profit_factor  = sa.profit_factor();
            row.total_win      = sa.total_win;
            row.total_loss     = sa.total_loss;
            auto lit = lots_by_strat.find(name);
            row.open_lots      = (lit != lots_by_strat.end()) ? lit->second : 0;
            auto bit = brackets_by_strat.find(name);
            row.armed_brackets = (bit != brackets_by_strat.end()) ? bit->second : 0;
            out.strategies.push_back(std::move(row));
        }
    }

    // Trend strip — equity + drawdown tails the Overview panel renders
    // as sparklines. 60 cells matches the panel width budget; longer
    // tails are sub-sampled by ascii::sparkline anyway. The rate tail
    // is sourced by the panel directly from ConsoleDashboard (it owns
    // the rolling rate_ema_ samples), so the engine doesn't reach into
    // the dashboard from here.
    constexpr std::size_t kTailWidth = 60;
    out.trend.equity_tail   = analytics_.equity_tail(kTailWidth);
    out.trend.drawdown_tail = analytics_.drawdown_tail(kTailWidth);

    out.trend.equity_now = out.equity;
    out.trend.equity_change_pct = (out.initial_balance > 0.0)
        ? (out.equity / out.initial_balance - 1.0) * 100.0
        : 0.0;
    out.trend.drawdown_now_pct = out.trend.drawdown_tail.empty()
        ? 0.0 : out.trend.drawdown_tail.back();
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

        // Backtest determinism: the async MM worker replenishes the book on
        // its own schedule, so the book state at any bar — and therefore
        // fill prices — would depend on thread scheduling. Backtests use
        // the inline replenish path instead (same as the other presets);
        // the async worker stays available for shadow/live.
        const bool async_mm = (config_.mode != engine_mode::backtest);
        if (async_mm)
        {
            mm_ring_ = std::make_shared<EventRing>();
            mm_order_ring_ = std::make_shared<MMRing>();
        }

        logging_worker_ = make_logging_worker();
        risk_worker_ = std::make_unique<RiskWorker>(risk_manager_, halt_flag_,
            config_.initial_balance, config_.rolling_window, config_.risk_free_rate,
            config_.periods_per_year, config_.max_equity_points);
        stats_worker_ = std::make_unique<StatsWorker>(config_.initial_balance, 1000,
            config_.rolling_window, config_.risk_free_rate,
            config_.periods_per_year, config_.max_equity_points);
        if (async_mm)
            mm_worker_ = std::make_unique<MarketMakerWorker>(
                config_.seed != 0 ? static_cast<unsigned>(config_.seed + 3) : 42u,
                *mm_order_ring_,
                mm_calibration{config_.mm_levels_per_side,
                               config_.mm_base_depth,
                               config_.mm_base_spread_pct,
                               config_.mm_vol_spread_mult,
                               config_.mm_max_half_spread_pct});
        wire_failure(*logging_worker_);
        wire_failure(*risk_worker_);
        wire_failure(*stats_worker_);
        if (mm_worker_) wire_failure(*mm_worker_);

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

        if (mm_worker_)
        {
            worker_threads_.emplace_back([this]() {
                mm_worker_->run(*mm_ring_);
            });
            pin_to_core(worker_threads_.back(), find_core(core_role::market_maker));
        }
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
        // Stop the watchdog before kill-switch begins. Otherwise its
        // poll thread can race against the kill-switch's halt_flag_
        // observation: heartbeat thread (provider-owned) is about to
        // be torn down, so its liveness atomic will go stale during
        // the kill-switch's REST sequence — we don't want that to
        // re-trigger halt as a "watchdog said the heartbeat hung."
        if (worker_watchdog_) worker_watchdog_->stop();

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

    std::size_t total_drops = logging_drops_ + risk_drops_ + stats_drops_
                            + observer_drops_ + risk_stats_drops_ + mm_drops_;
    if (total_drops > 0)
    {
        std::cerr << "  WARNING: " << total_drops << " events dropped from ring buffers.\n";
        std::cerr << "  Drops: logging=" << logging_drops_
                  << " risk=" << risk_drops_
                  << " stats=" << stats_drops_
                  << " observer=" << observer_drops_
                  << " risk_stats=" << risk_stats_drops_
                  << " mm=" << mm_drops_
                  << "\n";
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
}

#ifdef HAS_QUESTDB
void engine::questdb_begin()
{
    if (!config_.persist_enabled) return;

    truetest::questdb::StoreConfig scfg;
    scfg.host = config_.questdb_host;
    scfg.ilp_port = config_.questdb_ilp_port;
    scfg.http_port = config_.questdb_http_port;
    try
    {
        scfg.run_tag = truetest::questdb::make_run_tag(config_.run_tag);
    }
    catch (const std::exception& e)
    {
        std::cerr << "  WARNING: invalid --run-tag (" << e.what()
                  << ") — persistence disabled.\n";
        return;
    }

    scfg.mode = (config_.mode == engine_mode::backtest ? "backtest"
              :  config_.mode == engine_mode::shadow   ? "shadow"
              :                                          "live");
    // Engine_core has no TT_TARGET define, so derive a binary label from
    // the runtime mode. main.inc could supply a more accurate value, but
    // mode-based naming is informative enough for runs_meta.
    scfg.binary = (config_.mode == engine_mode::backtest ? "engine_backtest"
                :  config_.mode == engine_mode::shadow   ? "engine_shadow"
                :                                          "engine_live");
    scfg.strategy = primary_strategy_name_;
    if (data_handler_ && !data_handler_->db_data_symbol.empty())
        scfg.symbol = data_handler_->db_data_symbol.front();
    scfg.initial_equity = config_.initial_balance;
    scfg.notes = config_.run_notes;
    scfg.strict = config_.questdb_strict;
    scfg.ttl_days = 0; // Phase 4: can be extended to config if needed (default no TTL)

    if (config_.questdb_strict && !config_.questdb_fallback_path.empty())
        scfg.fallback_path = config_.questdb_fallback_path;
    else if (config_.questdb_strict && !scfg.run_tag.empty())
    {
        // Auto-generate a sensible fallback filename next to the binary log if possible
        scfg.fallback_path = scfg.run_tag + ".questdb_fallback.ilp";
    }

    questdb_store_ = std::make_shared<truetest::questdb::QuestdbStore>(
        std::move(scfg));

    if (questdb_store_->begin())
    {
        questdb_active_ = true;
        // Set to "now - cadence" so the very first maybe_questdb_tick() after activation
        // will consider flushing promptly (improves reliability for short runs / early data).
        last_questdb_flush_ = std::chrono::steady_clock::now() - config_.questdb_flush_cadence;
        std::cerr << "  QuestDB persistence ENABLED (run_tag="
                  << questdb_store_->run_tag() << ")\n";
    }
    else
    {
        if (config_.questdb_strict)
        {
            std::cerr << "\n  FATAL (strict mode): QuestDB unreachable at "
                      << config_.questdb_host << ":" << config_.questdb_http_port << "\n"
                      << "  --persist-strict requires a working QuestDB instance.\n"
                      << "  Start QuestDB (e.g. `questdb start`) and retry, or remove --persist-strict.\n\n";
            // Hard exit for strict mode
            std::exit(1);
        }
        else
        {
            std::cerr << "  WARNING: QuestDB unreachable at "
                      << config_.questdb_host << ":" << config_.questdb_http_port
                      << " — continuing with persistence DISABLED for this session.\n"
                      << "  Start the daemon with: questdb start\n"
                      << "  Or re-run without --persist to suppress this warning.\n";
            questdb_store_.reset();
        }
    }
}

void engine::questdb_end()
{
    if (!questdb_active_ || !questdb_store_) return;
    const auto report = analytics_.snapshot();
    const double final_equity = portfolio_.get_equity(last_mid_price_);
    questdb_store_->end(final_equity,
                        report.total_orders,
                        report.total_fills,
                        questdb_total_rejections_,
                        // Phase 4: richer campaign summary for long runs
                        report.max_drawdown,
                        report.sharpe_ratio,
                        report.sortino_ratio,
                        report.profit_factor,
                        report.win_rate,
                        report.calmar_ratio,
                        report.total_trades,
                        report.winning_trades);
    // Best-effort final flush of any remaining ILP buffer on clean shutdown.
    // This improves data completeness for the final rows of a run.
    questdb_store_->flush();
    questdb_active_ = false;
}
#endif

#ifdef HAS_QUESTDB
void engine::maybe_questdb_tick()
{
    if (!questdb_active_ || !questdb_store_) return;
    auto now = std::chrono::steady_clock::now();
    if (now - last_questdb_flush_ >= config_.questdb_flush_cadence) {
        questdb_store_->tick();
        last_questdb_flush_ = now;
    }
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

    // Queue-position telemetry (shadow + --queue-model l2-snapshot only).
    if (config_.mode == engine_mode::shadow && config_.provider)
    {
        auto exec = config_.provider->get_execution_adapter();
        if (auto* ts = dynamic_cast<TradeTapeShadowAdapter*>(exec.get()))
        {
            const auto qs = ts->get_queue_stats();
            if (qs.submitted_with_queue > 0)
            {
                std::cout << "  Queue model (shadow):\n"
                          << "    Submitted with queue ahead: " << qs.submitted_with_queue << "\n"
                          << "    Filled after queue drained: " << qs.filled_after_drain << "\n"
                          << "    Still queue-blocked at EOS: " << qs.blocked_at_eos << "\n";
            }
        }
    }

    // Maker queue telemetry (paper/backtest + --maker-queue-model).
    {
        std::size_t total_live = 0;
        double      total_qpos = 0.0;
        int         n = 0;

        for (auto& [_, ad] : execution_adapters_)
        {
            if (ad)
            {
                auto c = ad->live_quote_count();
                if (c > 0)
                {
                    total_live += c;
                    total_qpos += ad->avg_queue_position_bps();
                    ++n;
                }
            }
        }
        if (config_.provider)
        {
            auto pa = config_.provider->get_execution_adapter();
            if (pa)
            {
                auto c = pa->live_quote_count();
                if (c > 0)
                {
                    total_live += c;
                    total_qpos += pa->avg_queue_position_bps();
                    ++n;
                }
            }
        }

        if (total_live > 0)
        {
            uint32_t avg_bps = static_cast<uint32_t>(total_qpos / n);
            std::cout << "  Maker queue model:\n"
                      << "    Live passive limits: " << total_live << "\n"
                      << "    Avg queue position:  " << (avg_bps / 100) << "%\n";
            // Phase 2 richer stats (mainly for shadow TradeTape)
            auto pa = config_.provider ? config_.provider->get_execution_adapter() : nullptr;
            if (pa) {
                auto sub = pa->queue_submitted_with_queue();
                auto fil = pa->queue_filled_after_drain();
                auto blk = pa->queue_blocked_at_eos();
                if (sub > 0 || fil > 0 || blk > 0) {
                    std::cout << "    Queue detailed (shadow): submitted=" << sub
                              << " filled_after_drain=" << fil
                              << " blocked_at_eos=" << blk << "\n";
                }
            }
        }
    }

    // Dual Portfolio Shadow Report (Phase 2 - text version for non-TUI runs)
    if (config_.mode == engine_mode::shadow)
    {
        const portfolio* exch = get_exchange_portfolio();
        const Analytics* exch_analytics = get_exchange_analytics();

        if (exch && exch_analytics)
        {
            double last_price = (last_mid_price_ > 0.0) ? last_mid_price_ : 0.0;

            double sim_equity   = portfolio_.get_equity(last_price);
            double exch_equity  = exch->get_equity(last_price);
            double delta        = exch_equity - sim_equity;
            double delta_pct    = (sim_equity > 0.0) ? (delta / sim_equity * 100.0) : 0.0;

            std::cout << "\n";
            std::cout << "  ============================================\n";
            std::cout << "    Dual Portfolio Shadow Report (Text)\n";
            std::cout << "  ============================================\n";
            std::cout << std::fixed << std::setprecision(2);
            std::cout << "    Sim Equity:      $" << sim_equity << "\n";
            std::cout << "    Exchange Equity: $" << exch_equity << "\n";
            std::cout << "    Delta:           $" << delta 
                      << " (" << (delta >= 0 ? "+" : "") << delta_pct << "%)\n";
            std::cout << "    Sim Cash:        $" << portfolio_.get_cash() << "\n";
            std::cout << "    Exchange Cash:   $" << exch->get_cash() << "\n";
        }
    }
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

const portfolio* engine::get_exchange_portfolio() const
{
    if (config_.mode != engine_mode::shadow || !exchange_portfolio_.has_value())
        return nullptr;
    return &exchange_portfolio_.value();
}

const Analytics* engine::get_exchange_analytics() const
{
    if (config_.mode != engine_mode::shadow || !exchange_analytics_.has_value())
        return nullptr;
    return &exchange_analytics_.value();
}

// See declaration in engine.h for documentation.
void engine::reset_for_next_trial(uint64_t new_seed)
{
    // Reset main portfolio (cash, positions, lots)
    portfolio_.reset();

    // Reset analytics (very expensive to recreate)
    // Use the configured initial balance when available (falls back to 10000 if not set yet)
    double initial = (config_.initial_balance > 0.0) ? config_.initial_balance : 10000.0;
    analytics_.reset(initial);

    // Reset exit manager
    exit_manager_.reset();

    // Reset order tracker (important for isolation)
    order_tracker_.reset();

    // Reset risk manager
    risk_manager_.reset();

    // Update seed in config for any RNGs
    config_.seed = new_seed;

    // Reset some counters / state
    last_mid_price_ = 0.0;
    last_mark_symbol_.clear();

    // Clear orderbook registry (L2 state from previous trial)
    orderbook_registry_.clear();

    // Reset market maker and adverse selection trackers
    market_maker_.reset();
    adverse_selection_.reset();

    // Reset per-symbol caches that can leak state between trials
    instrument_cache_.clear();
    l2_seeded_symbols_.clear();

    // Reset tick aggregator (prevents partial bar leakage between trials)
    if (tick_aggregator_)
    {
        tick_aggregator_->reset();
    }

    // Clear UI/dashboard caches (harmless and cheap for headless MC runs)
    open_orders_cache_.clear();
    recent_fills_cache_.clear();

    // Phase 4 MC reuse hardening: clear order_meta_ for clean per-trial isolation
    // (opener/strategy attribution must not leak between independent trials).
    order_meta_.clear();

    // Clear shadow_tracker for per-trial isolation (divergence tracking not
    // relevant across MC trials; see MC controller comment).
    if (shadow_tracker_) shadow_tracker_->reset();

    // Note: Rings, workers, event_logger_, and dashboard timing are left mostly
    // untouched (as before). Workers repopulate via ring events on next fills/orders.
    // Full ring/worker reset is complex and usually unnecessary for MC reuse of
    // the engine instance (Phase B). Core objects (portfolio, analytics, exit_manager,
    // etc.) are now fully reset to enable broader reuse.
}

bool engine::process_order(const std::shared_ptr<order_event>& o,
                           std::size_t& event_count,
                           bool& halt_requested)
{
    // ========================================================================
    // CANONICAL HOT-PATH ORDERING (Phase 3 deepdive cleanup)
    // This documents the enforced sequence for order + fill processing.
    // All run_* paths (bar/tick/stream/replay), evaluate_exits, unwind, etc.
    // should follow this for consistent per-lot state, shadow divergence,
    // publish to rings/workers, and cache updates.
    //
    // 1. Venue pre-trade risk (FuturesRiskCheck / risk_check_) — reject only.
    // 2. RiskManager pre-order check (can halt).
    // 3. route_order (assigns id, register_order_meta for opener/strategy,
    //    instrument spec, stop pending, or submit).
    // 4. adapter->submit_order (paper or live); also submit to shadow provider
    //    adapter for dual tracking.
    // 5. adapter->poll_fills → for each fill:
    //      - stamp_fill_attribution (rich opener/strategy from meta or fe)
    //      - order_tracker / cache status
    //      - log + publish order status if needed
    //      - portfolio_.on_fill (rich, with opener/strategy)  [core lot update]
    //      - dispatch_fill_to_strategy
    //      - adverse_selection_.on_fill
    //      - exit_manager_.on_fill (rich)  [arm/cancel brackets per opener]
    //      - risk_manager_.on_fill
    //      - QuestDB record_fill (rich)
    //      - notify_position_change_all (multi-lot aware)
    //      - publish_event(fill) + analytics_.on_event
    //      - shadow_tracker on_sim (if shadow)
    //      - post_fill risk check (can halt + unwind)
    // 6. Shadow dual: separate poll of provider's exchange_adapter fills →
    //      shadow_tracker on_exchange + exchange_portfolio on_fill (rich)
    //      + exchange_analytics
    // 7. evaluate_exits (price or bar) — can emit closes that recurse via
    //      route_order/process_order (keeps lot/opener discipline).
    // 8. Cache updates for dashboard (open_orders, recent_fills) + rings
    //      publish for workers (after core state for snapshot coherence).
    //
    // Invariants: order_meta_ registered before any fill can reference it.
    // L2 updates reach queue models before trades (via apply_l2 before
    // on_trade in adapters). No new allocs/JSON on hot path. Multi-lot uses
    // opener_order_id discipline; single-lot may use bulk cancel in notify.
    // ========================================================================

    {
        // Venue-specific pre-trade check (futures notional / leverage /
        // liquidation distance) runs first. Refusals here are pure
        // rejections — no halt semantics, since these caps describe
        // the operator's prudent-trading envelope, not a market-wide
        // risk-of-ruin trigger.
        if (risk_check_)
        {
            auto vd = risk_check_->evaluate(*o, portfolio_, last_mid_price_);
            if (!vd.allow)
            {
                const std::string reason_str =
                    "venue risk check refused: " + vd.reason;
                auto rej = acquire_pooled(rejection_pool_,
                    o->get_timestamp(), o->get_symbol(),
                    o->get_order_id(), reason_str);
                log_event(*rej);
                publish_event(rej);
#ifdef HAS_QUESTDB
                if (questdb_active_ && questdb_store_)
                {
                    questdb_store_->record_order_submitted(*o, "rejected");
                    questdb_store_->record_rejection(*o, "venue_risk_reject",
                                                     reason_str.c_str());
                    questdb_total_rejections_++;
                }
#endif
                order_tracker_.set_status(o->get_order_id(),
                                          order_status::rejected);
                erase_open_order(o->get_order_id());
                // Reject, not halt — engine continues. The cap describes
                // what this operator considers prudent, not a market-wide
                // risk-of-ruin condition that should stop everything.
                return true;
            }
        }

        auto snap = analytics_.risk_view();
        auto action = risk_manager_.check_order(*o, portfolio_, snap);
        if (action == risk_action::halt || action == risk_action::reject)
        {
            const char* reason = (action == risk_action::halt)
                ? "risk limit breached - engine halted"
                : "order rejected by risk manager";

            auto rej = acquire_pooled(rejection_pool_,
                o->get_timestamp(), o->get_symbol(), o->get_order_id(), reason);
            log_event(*rej);
            publish_event(rej);

#ifdef HAS_QUESTDB
            if (questdb_active_ && questdb_store_)
            {
                questdb_store_->record_order_submitted(*o, "rejected");
                questdb_store_->record_rejection(*o,
                    (action == risk_action::halt) ? "risk_halt" : "risk_reject",
                    reason);
                questdb_store_->record_event(
                    "risk_decision",
                    o->get_symbol(),
                    o->get_strategy_name(),
                    o->get_order_id(),
                    (action == risk_action::halt) ? "halt" : "reject",
                    reason,
                    "{}"  // could be extended with more JSON context later
                );
                questdb_total_rejections_++;
            }
#endif

            order_tracker_.set_status(o->get_order_id(), order_status::rejected);
            erase_open_order(o->get_order_id());
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
    cache_open_order(*o);
    log_event(*o);
    publish_event(o);
    analytics_.on_event(o);

#ifdef HAS_QUESTDB
    if (questdb_active_ && questdb_store_)
    {
        questdb_store_->record_order_submitted(*o, "pending");
        questdb_store_->record_status_transition(o->get_order_id(),
            order_status::pending, order_status::open);
        questdb_store_->record_event(
            "order_intent",
            o->get_symbol(),
            o->get_strategy_name(),
            o->get_order_id(),
            "info",
            "order generated by strategy",
            "{}"
        );
    }
#endif

    auto adapter = get_adapter(o->get_symbol());

    if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
    {
        local->set_mid_price(last_mid_price_);
        local->set_l2_seeded(l2_seeded_symbols_.count(o->get_symbol()) > 0);
    }

    adapter->submit_order(*o);

    if (config_.mode == engine_mode::shadow && config_.provider)
    {
        auto exchange_adapter = config_.provider->get_execution_adapter();
        if (exchange_adapter)
            exchange_adapter->submit_order(*o);
    }

    if (!process_adapter_fills(adapter, event_count, halt_requested))
        return false;

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
                    stamp_fill_attribution(ef);

                    const uint64_t e_opener = ef.get_opener_order_id();
                    const std::string& e_strat = ef.get_strategy_name();

                    if (shadow_tracker_)
                        shadow_tracker_->on_exchange_fill(ef);

                    if (exchange_portfolio_.has_value())
                    {
                        exchange_portfolio_->on_fill(ef, e_opener, e_strat);
                    }
                }
            }
        }
    }

    event_count++;
    return true;
}

bool engine::process_adapter_fills(const std::shared_ptr<IExecutionAdapter>& adapter,
                                   std::size_t& event_count, bool& halt_requested)
{
    std::vector<fill_event> fills;
    if (!adapter->poll_fills(fills))
        return true;

    DEBUG_STAGE(stage_timer_, fill_processing);
    for (auto& f : fills)
    {
        stamp_fill_attribution(f);

        const uint64_t opener = f.get_opener_order_id();
        const std::string& strat = f.get_strategy_name();

        const auto new_status = f.is_partial()
            ? order_status::partially_filled : order_status::filled;
        order_tracker_.set_status(f.get_order_id(), new_status);
        cache_fill(f);
        if (f.is_partial())
            update_open_order_status(f.get_order_id(), "partial");
        else
            erase_open_order(f.get_order_id());
        auto fill_ptr = acquire_pooled(fill_pool_,f);
        log_event(f);
        portfolio_.on_fill(f, opener, strat);
        dispatch_fill_to_strategy(f);
        adverse_selection_.on_fill(f);
        exit_manager_.on_fill(f, opener);
        risk_manager_.on_fill(f);
#ifdef HAS_QUESTDB
        if (questdb_active_ && questdb_store_)
        {
            const char* src =
                (f.get_source() == fill_source::exchange)  ? "exchange"
              : (f.get_source() == fill_source::simulated) ? "simulated"
              :                                              "local";
            questdb_store_->record_fill(f, opener, strat, src);
            questdb_store_->record_status_transition(f.get_order_id(),
                order_status::open, new_status);
        }
#endif
        notify_position_change_all(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
        publish_event(fill_ptr);
        analytics_.on_event(fill_ptr);

        if (config_.mode == engine_mode::shadow && shadow_tracker_)
            shadow_tracker_->on_simulated_fill(f);

        event_count++;

        {
            auto post_snap = analytics_.risk_view();
            auto post_action = risk_manager_.check_post_fill(f, portfolio_, post_snap);
            if (post_action == risk_action::halt)
            {
                if (config_.risk_unwind)
                    unwind_positions(event_count);
                halt_requested = true;
                return false;
            }
        }
    }
    return true;
}

void engine::deliver_mm_book_trades(const std::string& symbol, const trades& trs,
                                    const std::chrono::system_clock::time_point& ts,
                                    std::size_t& event_count, bool& halt_requested)
{
    if (trs.empty())
        return;
    // Only adapters that already exist can hold resting strategy orders;
    // don't create one just to deliver MM-vs-MM crossings.
    auto it = execution_adapters_.find(symbol);
    if (it == execution_adapters_.end() || !it->second)
        return;
    auto* local = dynamic_cast<LocalBookAdapter*>(it->second.get());
    if (!local)
        return;
    local->on_book_trades(trs, ts);
    process_adapter_fills(it->second, event_count, halt_requested);
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
        erase_open_order(order_id);
        auto cancel_ev = acquire_pooled(cancel_pool_,
            std::chrono::system_clock::now(), symbol, order_id, reason);
        log_event(*cancel_ev);
        publish_event(cancel_ev);
        if (!config_.is_threaded())
            analytics_.on_event(cancel_ev);
#ifdef HAS_QUESTDB
        if (questdb_active_ && questdb_store_)
        {
            questdb_store_->record_cancellation(order_id, symbol,
                lookup_strategy_name(order_id),
                reason.empty() ? "manual" : reason);
            questdb_store_->record_status_transition(order_id,
                order_status::open, order_status::cancelled, reason);
        }
#endif
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
        const auto now = std::chrono::system_clock::now();
        auto amend_ev = acquire_pooled(amend_pool_,
            now, symbol, order_id, new_price, new_qty);
        log_event(*amend_ev);
        publish_event(amend_ev);
        if (!config_.is_threaded())
            analytics_.on_event(amend_ev);
#ifdef HAS_QUESTDB
        if (questdb_active_ && questdb_store_)
        {
            // Engine doesn't preserve old price/qty cleanly here; log zeros
            // and rely on the orders/order_status tables for history.
            questdb_store_->record_amendment(order_id, symbol,
                /*old_price=*/0.0, new_price,
                /*old_qty=*/0.0, new_qty, now);
        }
#endif
    }

    return modified;
}

void engine::unwind_positions(std::size_t& event_count)
{
    // Snapshot before iterating — each fill mutates positions_.
    std::vector<std::pair<std::string, double>> to_close;
    to_close.reserve(portfolio_.get_positions().size());
    for (const auto& [symbol, pos] : portfolio_.get_positions())
    {
        if (std::abs(pos.qty) >= 1e-12)
            to_close.emplace_back(symbol, pos.qty);
    }

    for (const auto& [symbol, qty] : to_close)
    {
        // Sign-aware flatten — shorts need market BUY, not SELL.
        const order_side close_side = (qty > 0.0)
            ? order_side::sell : order_side::buy;
        const double close_qty = std::abs(qty);

        auto now = std::chrono::system_clock::now();
        auto close_order = acquire_pooled(order_pool_,order_event(
            now, symbol, order_type::market, close_side,
            close_qty, last_mid_price_));
        close_order->set_order_id(OrderIdGenerator::next());
        close_order->set_strategy_name("risk_unwind");

        order_tracker_.set_status(close_order->get_order_id(), order_status::open);
        cache_open_order(*close_order);
        log_event(*close_order);
        publish_event(close_order);
        analytics_.on_event(close_order);

#ifdef HAS_QUESTDB
        if (questdb_active_ && questdb_store_)
        {
            questdb_store_->record_order_submitted(*close_order, "pending");
            questdb_store_->record_status_transition(close_order->get_order_id(),
                order_status::pending, order_status::open, "risk_unwind");
        }
#endif

        auto adapter = get_adapter(symbol);
        if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
        {
            local->set_mid_price(last_mid_price_);
            local->set_l2_seeded(l2_seeded_symbols_.count(symbol) > 0);
        }

        adapter->submit_order(*close_order);

        std::vector<fill_event> fills;
        if (adapter->poll_fills(fills))
        {
            for (auto& f : fills)
            {
                stamp_fill_attribution(f);

                const uint64_t opener = f.get_opener_order_id();
                const std::string& strat = f.get_strategy_name();

                const auto new_status = f.is_partial()
                    ? order_status::partially_filled : order_status::filled;
                order_tracker_.set_status(f.get_order_id(), new_status);
                cache_fill(f);
                if (f.is_partial())
                    update_open_order_status(f.get_order_id(), "partial");
                else
                    erase_open_order(f.get_order_id());
                auto fill_ptr = acquire_pooled(fill_pool_,f);
                log_event(f);
                portfolio_.on_fill(f, opener, strat);
            dispatch_fill_to_strategy(f);
                adverse_selection_.on_fill(f);
                exit_manager_.on_fill(f, opener);
                risk_manager_.on_fill(f);
#ifdef HAS_QUESTDB
                if (questdb_active_ && questdb_store_)
                {
                    const char* src =
                        (f.get_source() == fill_source::exchange)  ? "exchange"
                      : (f.get_source() == fill_source::simulated) ? "simulated"
                      :                                              "local";
                    questdb_store_->record_fill(f, opener, strat, src);
                    questdb_store_->record_status_transition(f.get_order_id(),
                        order_status::open, new_status, "risk_unwind");
                }
#endif
                notify_position_change_all(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                publish_event(fill_ptr);
                analytics_.on_event(fill_ptr);

                event_count++;
            }
        }
    }
}

void engine::apply_l2_snapshot(const std::string& symbol,
                               const std::vector<l2_level>& bids,
                               const std::vector<l2_level>& asks)
{
    drain_object_pool_returns();

    // Tag L2-driven so MarketMaker::replenish stops seeding paper depth.
    l2_seeded_symbols_.insert(symbol);
    auto ob = orderbook_registry_.get_or_create(symbol);

    const std::size_t n_bids =
        std::min(bids.size(), kL2SnapshotMaxLevels);
    const std::size_t n_asks =
        std::min(asks.size(), kL2SnapshotMaxLevels);

    std::array<std::pair<Price, quantity>, kL2SnapshotMaxLevels> ob_bids{};
    std::array<std::pair<Price, quantity>, kL2SnapshotMaxLevels> ob_asks{};
    std::array<std::pair<double, double>, kL2SnapshotMaxLevels> abids{};
    std::array<std::pair<double, double>, kL2SnapshotMaxLevels> aasks{};

    for (std::size_t i = 0; i < n_bids; ++i)
    {
        ob_bids[i] = {Price::from_double(bids[i].price),
                      static_cast<quantity>(bids[i].quantity)};
        abids[i] = {bids[i].price, static_cast<double>(bids[i].quantity)};
    }
    for (std::size_t i = 0; i < n_asks; ++i)
    {
        ob_asks[i] = {Price::from_double(asks[i].price),
                      static_cast<quantity>(asks[i].quantity)};
        aasks[i] = {asks[i].price, static_cast<double>(asks[i].quantity)};
    }

    ob->apply_l2_snapshot(ob_bids.data(), n_bids, ob_asks.data(), n_asks);
    refresh_top_of_book_atomics(*ob);

    // Forward L2 to execution adapters so QueueAwareBookAdapter (paper) and
    // TradeTapeShadowAdapter (shadow queue model) can maintain level aggregates
    // / queue_ahead. Central place for all L2-driven paths (direct apply, replay,
    // streaming). Duplicated in provider event dispatch for the live shadow_exec
    // case; keep in sync.
    std::vector<std::pair<double, double>> abid_vec(abids.begin(),
                                                    abids.begin() + n_bids);
    std::vector<std::pair<double, double>> aask_vec(aasks.begin(),
                                                    aasks.begin() + n_asks);
    for (auto& [_, ad] : execution_adapters_)
        if (ad) ad->on_l2_snapshot(symbol, abid_vec, aask_vec);
    if (config_.provider)
        if (auto pa = config_.provider->get_execution_adapter())
            pa->on_l2_snapshot(symbol, abid_vec, aask_vec);

    auto ev = acquire_pooled(l2_snapshot_pool_,
        std::chrono::system_clock::now(), symbol,
        bids.data(), n_bids, asks.data(), n_asks);
    log_event(*ev);
    publish_event(ev);
}

void engine::apply_l2_update(const std::string& symbol,
                             tick_side ts_side, double price, int64_t new_qty)
{
    drain_object_pool_returns();

    auto ob = orderbook_registry_.get_or_create(symbol);

    side ob_side = (ts_side == tick_side::bid) ? side::buy : side::sell;
    ob->apply_l2_update(ob_side, Price::from_double(price),
                        static_cast<quantity>(new_qty));
    refresh_top_of_book_atomics(*ob);

    // Forward L2 update to adapters for queue models (see apply_l2_snapshot).
    const order_side os = (ts_side == tick_side::bid) ? order_side::buy : order_side::sell;
    for (auto& [_, ad] : execution_adapters_)
        if (ad) ad->on_l2_update(symbol, os, price, static_cast<double>(new_qty));
    if (config_.provider)
        if (auto pa = config_.provider->get_execution_adapter())
            pa->on_l2_update(symbol, os, price, static_cast<double>(new_qty));

    auto ev = acquire_pooled(l2_update_pool_,
        std::chrono::system_clock::now(), symbol, ts_side, price, new_qty);
    log_event(*ev);
    publish_event(ev);

    /* LIVE_SAFETY_CCB_APPROVED: Minimal non-invasive dispatch of l2_update
       to IStrategy (primary + additional). This enables L2-driven HFT
       strategies such as AdaptiveHybridStrategy without touching
       halt_flag_, risk paths, live-order gates, TT_TARGET, reconciler,
       kill-switch, or any other Phase-1 frozen surface.
       Dispatch occurs after apply + publish (same thread as on_tick/on_market).
       Two-person CCB + clean 4-hour engine_shadow run required before merge.
       See CLAUDE.md and docs/production-readiness-gaps.md. */
    if (!pause_all_.load(std::memory_order_acquire) &&
        !halt_flag_.load(std::memory_order_acquire))
    {
        size_t l2_event_count = 0;
        if (strategy_) {
            if (auto o = strategy_->on_l2_update(*ev)) {
                o->set_recv_ns(0); // TODO: wire real ingress ns in future patch
                o->set_strategy_name(primary_strategy_name_);
                bool dummy_halt = false;
                route_order(*o, ev->get_timestamp(), l2_event_count, dummy_halt);
                if (!dummy_halt)
                    register_strategy_exit_intent(*strategy_, primary_strategy_name_, o->get_order_id());
            }
        }
        for (std::size_t i = 0; i < additional_strategies_.size(); ++i) {
            auto& s = additional_strategies_[i];
            if (s) {
                if (auto o = s->on_l2_update(*ev)) {
                    o->set_strategy_name(additional_strategy_names_[i]);
                    bool dummy_halt = false;
                    route_order(*o, ev->get_timestamp(), l2_event_count, dummy_halt);
                }
            }
        }
    }
}

void engine::refresh_top_of_book_atomics(const orderbook& ob)
{
    auto* dash = config_.dashboard.get();
    if (!dash) return;
    // bids descending, asks ascending — front() is top of book.
    auto& st = dash->stats();
    const auto infos = ob.get_order_infos();
    const auto& bids = infos.get_bids();
    const auto& asks = infos.get_asks();
    st.best_bid_fp8.store(
        bids.empty() ? std::int64_t{-1}
                     : static_cast<std::int64_t>(
                           std::llround(bids.front().price_.to_double() * 1e8)),
        std::memory_order_relaxed);
    st.best_ask_fp8.store(
        asks.empty() ? std::int64_t{-1}
                     : static_cast<std::int64_t>(
                           std::llround(asks.front().price_.to_double() * 1e8)),
        std::memory_order_relaxed);
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
                         std::size_t& event_count, bool& halt_requested,
                         bool anchor_immediate)
{
    // Operator-pause gate: intercept here so every strategy call site is
    // covered by one branch. Strategies still run (so analytics + lots
    // stay live for fills already in flight), but no new orders reach
    // the venue. The intent's order_id stays 0, which makes
    // register_strategy_exit_intent a no-op for paused emissions.
    if (pause_all_.load(std::memory_order_acquire))
    {
        (void)sim_time; (void)event_count; (void)halt_requested;
        return true;
    }

    order.set_order_id(OrderIdGenerator::next());
    order_tracker_.set_status(order.get_order_id(), order_status::pending);
    // Canonical step: register_order_meta before any submit or potential fill.
    // This populates opener/strategy so stamp_fill_attribution and rich
    // on_fill paths have the data (critical for per-lot and multi-lot).
    register_order_meta(order);

    if (auto* spec = resolve_instrument_spec(order.get_symbol()))
    {
        if (!apply_instrument_spec(order, *spec))
        {
            const char* reason = "order rejected by venue filter (min_qty/min_notional)";
            auto rej = acquire_pooled(rejection_pool_,
                order.get_timestamp(), order.get_symbol(),
                order.get_order_id(), reason);
            log_event(*rej);
            publish_event(rej);
#ifdef HAS_QUESTDB
            if (questdb_active_ && questdb_store_)
            {
                questdb_store_->record_order_submitted(order, "rejected");
                questdb_store_->record_rejection(order, "venue_filter", reason);
                questdb_total_rejections_++;
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
        pending_stops_.push_back(acquire_pooled(order_pool_,order));
        return true;
    }

    if (anchor_immediate)
    {
        // Bracket fire: fill where it triggered. Re-center the synthetic
        // book at the anchored fire price (SL/TP level or gap open) within
        // the trigger bar and execute immediately — deferring through
        // execution_bar_delay would discard the fire price and fill at
        // wherever the next bar happens to open. Same convention as the
        // native stop path in check_pending_stops.
        const double ref = order.get_price();
        const double bar_mid = last_mid_price_;
        if (ref > 0.0 && ref != bar_mid)
        {
            auto ob = orderbook_registry_.get_or_create(order.get_symbol());
            if (!mm_worker_ &&
                !l2_seeded_symbols_.count(order.get_symbol()))
            {
                auto mm_trades = market_maker_.replenish(
                    ob, ref, /*update_history=*/false);
                deliver_mm_book_trades(order.get_symbol(), mm_trades,
                                       sim_time, event_count, halt_requested);
            }
            last_mid_price_ = ref;
        }
        order.set_earliest_eligible_ts(sim_time);
        auto order_ptr = acquire_pooled(order_pool_,order);
        if (order.get_tif() == time_in_force::day)
            day_order_ids_.push_back({order.get_symbol(), order.get_order_id()});
        const bool ok = process_order(order_ptr, event_count, halt_requested);
        last_mid_price_ = bar_mid;
        return ok;
    }

    if (config_.latency_model)
    {
        auto latency = config_.latency_model->get_order_latency();
        order.set_earliest_eligible_ts(sim_time + latency);
        pending_orders_.push({acquire_pooled(order_pool_,order), order_seq_++});
        return true;
    }

    if (config_.execution_bar_delay > 0)
    {
        order.set_earliest_eligible_ts(sim_time + std::chrono::nanoseconds(1));
        pending_orders_.push({acquire_pooled(order_pool_,order), order_seq_++});
        return true;
    }

    order.set_earliest_eligible_ts(sim_time);
    auto order_ptr = acquire_pooled(order_pool_,order);
    if (order.get_tif() == time_in_force::day)
        day_order_ids_.push_back({order.get_symbol(), order.get_order_id()});
    return process_order(order_ptr, event_count, halt_requested);
}

void engine::check_pending_stops(double open, double high, double low,
                                 const std::chrono::system_clock::time_point& sim_time,
                                 std::size_t& event_count, bool& halt_requested)
{
    // last_mid_price_ is the bar close when this runs in the bar loops;
    // restored after each anchored fill so subsequent processing keeps
    // the close reference. Tick callers pass open == high == low ==
    // last_mid_price_, making the anchor a no-op there.
    const double bar_mid = last_mid_price_;

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
            // Fill reference: the stop price, or the bar open when the
            // bar gapped through the stop. Never the close — that is
            // intra-bar look-ahead and deviates from the convention of
            // filling where the stop was hit.
            const double ref = (stop->get_side() == order_side::buy)
                ? ((open >= stop->get_stop_price()) ? open : stop->get_stop_price())
                : ((open <= stop->get_stop_price()) ? open : stop->get_stop_price());

            last_mid_price_ = ref;
            if (ref != bar_mid)
            {
                // Re-center the synthetic book at the trigger so the
                // converted order walks depth priced around ref, not
                // around the previous close. Skipped for real L2 depth
                // and under the threaded MM preset (the worker owns the
                // book there).
                auto ob = orderbook_registry_.get_or_create(stop->get_symbol());
                if (!mm_worker_ &&
                    !l2_seeded_symbols_.count(stop->get_symbol()))
                {
                    auto mm_trades = market_maker_.replenish(
                        ob, ref, /*update_history=*/false);
                    deliver_mm_book_trades(stop->get_symbol(), mm_trades,
                                           sim_time, event_count, halt_requested);
                }
            }

            if (stop->get_order_type() == order_type::stop)
            {
                auto market_order = acquire_pooled(order_pool_,
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
                auto limit_order = acquire_pooled(order_pool_,
                    sim_time, stop->get_symbol(), order_type::limit,
                    stop->get_side(), stop->get_quantity(), stop->get_price(),
                    stop->get_tif());
                limit_order->set_order_id(stop->get_order_id());
                limit_order->set_earliest_eligible_ts(sim_time);
                if (limit_order->get_tif() == time_in_force::day)
                    day_order_ids_.push_back({limit_order->get_symbol(), limit_order->get_order_id()});
                process_order(limit_order, event_count, halt_requested);
            }
            last_mid_price_ = bar_mid;
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
    // Legacy net-truth push for strategies that still override
    // set_position_open. Multi-lot strategies ignore this and track
    // openers via on_fill.
    if (strategy_) strategy_->set_position_open(symbol, open);
    for (auto& s : additional_strategies_)
        if (s) s->set_position_open(symbol, open);

    // On a net-flat transition, sweep any leftover bracket for a
    // single-lot strategy. This catches strategies that close via a
    // signal in on_market/on_tick without setting opener_order_id on the
    // closer — the per-opener cancel path in ExitManager::on_fill can't
    // reach those, so the intent would otherwise stay armed and could
    // fire later as a phantom close on a flat position. Skipped when
    // multiple openers are live for a (strategy,symbol) — that's the
    // multi-lot pattern, where the strategy owns opener_order_id
    // discipline and we must not bulk-cancel.
    if (!open)
    {
        auto sweep = [&](const std::string& name) {
            if (name.empty()) return;
            if (exit_manager_.openers_for(name, symbol) <= 1)
                exit_manager_.cancel(name, symbol);
        };
        sweep(primary_strategy_name_);
        for (const auto& name : additional_strategy_names_)
            sweep(name);
    }
}

void engine::register_order_meta(const order_event& o)
{
    const std::uint64_t opener = (o.get_opener_order_id() != 0)
        ? o.get_opener_order_id()
        : o.get_order_id();
    order_meta_[o.get_order_id()] = order_meta{opener, o.get_strategy_name()};
}

std::uint64_t engine::lookup_opener(std::uint64_t order_id) const
{
    auto it = order_meta_.find(order_id);
    return it != order_meta_.end() ? it->second.opener_order_id : 0;
}

const std::string& engine::lookup_strategy_name(std::uint64_t order_id) const
{
    static const std::string empty;
    auto it = order_meta_.find(order_id);
    return it != order_meta_.end() ? it->second.strategy_name : empty;
}

void engine::stamp_fill_attribution(fill_event& f)
{
    // Phase 1 deepdive: ensure every fill carries opener + strategy for
    // consistent per-lot bookkeeping across portfolio, ExitManager, QuestDB,
    // workers (via rings), shadow duals, analytics, and dashboard snapshot.
    if (f.get_opener_order_id() == 0)
    {
        if (auto op = lookup_opener(f.get_order_id()); op != 0)
            f.set_opener_order_id(op);
    }
    if (f.get_strategy_name().empty())
    {
        const auto& sn = lookup_strategy_name(f.get_order_id());
        if (!sn.empty())
            f.set_strategy_name(sn);
    }
}

void engine::dispatch_fill_to_strategy(const fill_event& f)
{
    const std::string& name = lookup_strategy_name(f.get_order_id());
    if (name.empty()) return;
    const std::uint64_t opener = lookup_opener(f.get_order_id());

    if (strategy_ && name == primary_strategy_name_)
    {
        strategy_->on_fill(f, opener);
        return;
    }
    for (std::size_t i = 0; i < additional_strategies_.size(); ++i)
    {
        if (i < additional_strategy_names_.size() &&
            additional_strategy_names_[i] == name &&
            additional_strategies_[i])
        {
            additional_strategies_[i]->on_fill(f, opener);
            return;
        }
    }
}

void engine::drain_venue_bracket_meta()
{
    if (!config_.provider) return;
    auto adapter = config_.provider->get_execution_adapter();
    auto* bridge = dynamic_cast<ExecutionBridge*>(adapter.get());
    if (!bridge) return;

    std::vector<ExecutionBridge::synth_meta> meta;
    if (!bridge->poll_synth_meta(meta)) return;

    for (const auto& m : meta)
    {
        order_meta_[m.engine_order_id] = order_meta{
            m.opener_order_id, m.strategy_name};
    }
}

void engine::register_strategy_exit_intent(IStrategy& strategy,
                                           const std::string& strategy_name,
                                           std::uint64_t order_id)
{
    if (order_id == 0) return;  // opener not yet assigned — cannot key
    auto intents = strategy.take_pending_exit_intents();
    for (auto& intent : intents)
    {
        intent.opener_order_id = order_id;
        intent.strategy_name   = strategy_name;
        exit_manager_.register_pending(std::move(intent));
    }
}

bool engine::evaluate_exits(const std::string& symbol, double px,
                            std::chrono::system_clock::time_point ts,
                            std::size_t& event_count,
                            std::int64_t recv_ns)
{
    // See canonical sequence comment in process_order. Closes emitted here
    // go through route_order (which registers meta) + process_order to
    // maintain per-lot / opener discipline and full state propagation.
    auto closes = exit_manager_.on_price(symbol, px, ts);
    if (closes.empty()) return false;
    for (auto& close : closes)
    {
        close.set_recv_ns(recv_ns);
        bool halt = false;
        route_order(close, ts, event_count, halt, /*anchor_immediate=*/true);
        if (halt) return true;
    }
    return false;
}

bool engine::evaluate_exits(const std::string& symbol,
                            double open, double low, double high, double close,
                            std::chrono::system_clock::time_point ts,
                            std::size_t& event_count,
                            std::int64_t recv_ns)
{
    // See canonical sequence comment in process_order. Bar fires go through
    // route_order for consistent meta registration and full propagation,
    // anchored at the fire price computed within the trigger bar.
    auto fires = exit_manager_.on_bar(symbol, open, low, high, close, ts);
    if (fires.empty()) return false;
    for (auto& c : fires)
    {
        c.set_recv_ns(recv_ns);
        bool halt = false;
        route_order(c, ts, event_count, halt, /*anchor_immediate=*/true);
        if (halt) return true;
    }
    return false;
}

void engine::sweep_resting_limits(const std::string& symbol,
                                  double low, double high,
                                  const std::chrono::system_clock::time_point& ts,
                                  std::size_t& event_count, bool& halt_requested)
{
    auto it = execution_adapters_.find(symbol);
    if (it == execution_adapters_.end() || !it->second)
        return;
    auto* local = dynamic_cast<LocalBookAdapter*>(it->second.get());
    if (!local)
        return;
    if (local->sweep_resting_range(symbol, low, high, ts))
        process_adapter_fills(it->second, event_count, halt_requested);
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

        if (evaluate_exits(mkt.get_symbol(),
                           mkt.get_open(), mkt.get_low(), mkt.get_high(), mkt.get_close(),
                           ts, event_count, mkt.get_recv_ns()))
            return;

        if (auto o = s->on_market(mkt))
        {
            o->set_recv_ns(mkt.get_recv_ns());
            o->set_strategy_name(additional_strategy_names_[i]);
            bool halt = false;
            route_order(*o, ts, event_count, halt);
            if (!halt)
                register_strategy_exit_intent(*s, additional_strategy_names_[i], o->get_order_id());
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
            bool halt = false;
            route_order(*o, ts, event_count, halt);
            if (!halt)
                register_strategy_exit_intent(*s, additional_strategy_names_[i], o->get_order_id());
            if (halt) return;
        }
    }
}

void engine::write_adapter_diagnostics(truetest::ui::streaming_stats& st)
{
    std::uint32_t live = 0;
    std::uint64_t queue_sum = 0;
    std::uint32_t queue_n   = 0;
    auto collect = [&](IExecutionAdapter* a) {
        if (!a) return;
        const auto c = a->live_quote_count();
        if (c == 0) return;
        live      += static_cast<std::uint32_t>(c);
        queue_sum += a->avg_queue_position_bps();
        ++queue_n;
    };
    for (auto& [_, ad] : execution_adapters_)
        collect(ad.get());
    if (config_.provider)
        collect(config_.provider->get_execution_adapter().get());

    st.live_quotes.store(live, std::memory_order_relaxed);
    st.avg_queue_pos_bps.store(
        queue_n > 0 ? static_cast<std::uint32_t>(queue_sum / queue_n) : 0u,
        std::memory_order_relaxed);
}

void engine::process_single_bar(const bar_record& rec, std::size_t& event_count,
                                const std::chrono::system_clock::time_point& timestamp)
{
    drain_object_pool_returns();

    // Operator-requested flatten: drain on the next event so the timestamp
    // we close at is from the live stream rather than wall-clock-now.
    if (flatten_request_.exchange(false, std::memory_order_acq_rel))
        unwind_positions(event_count);

    // Advance adapter clocks first so cancels whose in-flight window has
    // elapsed are drained before this event's matching runs.
    for (auto& [_, ad] : execution_adapters_)
        if (ad) ad->advance_time(timestamp);
    if (config_.provider)
    {
        if (auto pa = config_.provider->get_execution_adapter())
            pa->advance_time(timestamp);
    }

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
    last_mark_symbol_ = mkt.get_symbol();

    // Re-center the synthetic book at the open before draining pending
    // orders: next-bar-open fills must walk depth priced at the open,
    // not at the previous close (visible on gap bars).
    if (!pending_orders_.empty() &&
        pending_orders_.top().order->get_earliest_eligible_ts() <= timestamp)
    {
        auto ob_open = orderbook_registry_.get_or_create(mkt.get_symbol());
        if (!mm_worker_ &&
            !l2_seeded_symbols_.count(mkt.get_symbol()))
        {
            auto mm_trades = market_maker_.replenish(
                ob_open, last_mid_price_, /*update_history=*/false);
            bool halt = false;
            deliver_mm_book_trades(mkt.get_symbol(), mm_trades,
                                   timestamp, event_count, halt);
        }
    }

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
    last_mark_symbol_ = mkt.get_symbol();

    {
        // Stops were previously never evaluated in streaming bar mode —
        // they silently never triggered. Local halt mirrors the pending
        // drain above; risk halts propagate via halt_flag_.
        bool halt = false;
        check_pending_stops(mkt.get_open(), mkt.get_high(), mkt.get_low(),
                            timestamp, event_count, halt);
        sweep_resting_limits(mkt.get_symbol(), mkt.get_low(), mkt.get_high(),
                             timestamp, event_count, halt);
    }

    auto ob = orderbook_registry_.get_or_create(mkt.get_symbol());
    if (!mm_worker_ &&
        !l2_seeded_symbols_.count(mkt.get_symbol()))
    {
        auto mm_trades = market_maker_.replenish(ob, last_mid_price_);
        bool halt = false;
        deliver_mm_book_trades(mkt.get_symbol(), mm_trades,
                               timestamp, event_count, halt);
    }

    if (config_.provider && config_.provider->has_execution())
    {
        config_.provider->on_mid_price(mkt.get_symbol(), last_mid_price_);
        auto provider_adapter = config_.provider->get_execution_adapter();

        // Shadow bar-path: feed a synthetic close+volume trade. Lossy
        // (no intra-bar path), but bar-only shadow is low-fidelity anyway.
        if (config_.mode == engine_mode::shadow && provider_adapter)
        {
            provider_adapter->on_trade(mkt.get_symbol(), mkt.get_close(),
                                       static_cast<double>(mkt.get_volume()),
                                       mkt.get_timestamp());
        }

        drain_venue_bracket_meta();
        std::vector<fill_event> provider_fills;
        if (provider_adapter && provider_adapter->poll_fills(provider_fills))
        {
            for (auto& f : provider_fills)
            {
                if (config_.mode == engine_mode::shadow)
                {
                    if (shadow_tracker_)
                        shadow_tracker_->on_exchange_fill(f);

                    if (exchange_portfolio_.has_value())
                    {
                        stamp_fill_attribution(f);
                        exchange_portfolio_->on_fill(f, f.get_opener_order_id(),
                                                   f.get_strategy_name());
                    }
                    if (exchange_analytics_.has_value())
                    {
                        // Feed to second analytics for equity curve / metrics
                        auto fill_ptr = acquire_pooled(fill_pool_,f);
                        exchange_analytics_->on_event(fill_ptr);
                    }
                    continue;
                }
                stamp_fill_attribution(f);

                const uint64_t opener = f.get_opener_order_id();
                const std::string& strat = f.get_strategy_name();

                cache_fill(f);
                erase_open_order(f.get_order_id());
                auto fill_ptr = acquire_pooled(fill_pool_,f);
                portfolio_.on_fill(f, opener, strat);
            dispatch_fill_to_strategy(f);
                adverse_selection_.on_fill(f);
                exit_manager_.on_fill(f, opener);
                notify_position_change_all(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                publish_event(fill_ptr);
                // Engine-local analytics always sees fills (cheap, rare) so
                // risk_view() cash/positions stay correct in threaded presets
                // — same contract as the batch path in process_order().
                analytics_.on_event(fill_ptr);
                event_count++;
            }
        }
    }

    auto mkt_ptr = acquire_pooled(market_pool_,mkt);
    auto order_opt = strategy_->on_market(mkt);
    if (order_opt) order_opt->set_recv_ns(mkt.get_recv_ns());
    log_event(mkt);
    publish_event(mkt_ptr);
    if (!config_.is_threaded())
        analytics_.on_event(mkt_ptr);
    else
        analytics_.on_mark(mkt.get_symbol(), mkt.get_close());
    event_count++;

    if (evaluate_exits(mkt.get_symbol(),
                       mkt.get_open(), mkt.get_low(), mkt.get_high(), mkt.get_close(),
                       timestamp, event_count, mkt.get_recv_ns()))
        return;

    if (order_opt)
    {
        if (!primary_strategy_name_.empty())
            order_opt->set_strategy_name(primary_strategy_name_);
        bool halt = false;
        route_order(*order_opt, timestamp, event_count, halt);
        if (!halt && strategy_)
            register_strategy_exit_intent(*strategy_, primary_strategy_name_, order_opt->get_order_id());
    }
    dispatch_extras_on_market(mkt, timestamp, event_count);
}

void engine::process_single_tick(const tick_record& rec, std::size_t& event_count)
{
    drain_object_pool_returns();

    if (flatten_request_.exchange(false, std::memory_order_acq_rel))
        unwind_positions(event_count);

    for (auto& [_, ad] : execution_adapters_)
        if (ad) ad->advance_time(rec.timestamp);
    if (config_.provider)
    {
        if (auto pa = config_.provider->get_execution_adapter())
            pa->advance_time(rec.timestamp);
    }

    tick_side ts = tick_side::unknown;
    if (rec.side == data_tick_side::bid) ts = tick_side::bid;
    else if (rec.side == data_tick_side::ask) ts = tick_side::ask;

    tick_event te(rec.timestamp, rec.symbol, rec.price, rec.quantity, ts);
    te.set_recv_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    last_mid_price_ = rec.price;
    last_mark_symbol_ = rec.symbol;

    {
        DEBUG_STAGE(stage_timer_, mm_replenish);
        auto ob = orderbook_registry_.get_or_create(rec.symbol);
        if (!l2_seeded_symbols_.count(rec.symbol))
        {
            auto mm_trades = market_maker_.replenish(ob, last_mid_price_);
            bool halt = false;
            deliver_mm_book_trades(rec.symbol, mm_trades,
                                   rec.timestamp, event_count, halt);
        }
    }

    if (config_.provider && config_.provider->has_execution())
    {
        config_.provider->on_mid_price(rec.symbol, last_mid_price_);
        auto provider_adapter = config_.provider->get_execution_adapter();

        // Must fire BEFORE poll_fills so fills this tick are drained here.
        if (config_.mode == engine_mode::shadow && provider_adapter)
        {
            provider_adapter->on_trade(rec.symbol, rec.price,
                                       static_cast<double>(rec.quantity),
                                       rec.timestamp);
        }

        drain_venue_bracket_meta();
        std::vector<fill_event> provider_fills;
        if (provider_adapter && provider_adapter->poll_fills(provider_fills))
        {
            for (auto& f : provider_fills)
            {
                if (config_.mode == engine_mode::shadow)
                {
                    if (shadow_tracker_)
                        shadow_tracker_->on_exchange_fill(f);

                    if (exchange_portfolio_.has_value())
                    {
                        stamp_fill_attribution(f);
                        exchange_portfolio_->on_fill(f, f.get_opener_order_id(),
                                                   f.get_strategy_name());
                    }
                    if (exchange_analytics_.has_value())
                    {
                        // Feed to second analytics for equity curve / metrics
                        auto fill_ptr = acquire_pooled(fill_pool_,f);
                        exchange_analytics_->on_event(fill_ptr);
                    }
                    continue;
                }
                stamp_fill_attribution(f);

                const uint64_t opener = f.get_opener_order_id();
                const std::string& strat = f.get_strategy_name();

                cache_fill(f);
                erase_open_order(f.get_order_id());
                auto fill_ptr = acquire_pooled(fill_pool_,f);
                portfolio_.on_fill(f, opener, strat);
            dispatch_fill_to_strategy(f);
                adverse_selection_.on_fill(f);
                exit_manager_.on_fill(f, opener);
                notify_position_change_all(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                publish_event(fill_ptr);
                // Engine-local analytics always sees fills (cheap, rare) so
                // risk_view() cash/positions stay correct in threaded presets
                // — same contract as the batch path in process_order().
                analytics_.on_event(fill_ptr);
                event_count++;
            }
        }
    }

    bool halt = false;

    {
        DEBUG_STAGE(stage_timer_, pending_drain);
        while (!pending_orders_.empty() &&
               pending_orders_.top().order->get_earliest_eligible_ts() <= rec.timestamp)
        {
            auto entry = pending_orders_.top();
            pending_orders_.pop();
            if (entry.order->get_tif() == time_in_force::day)
                day_order_ids_.push_back({entry.order->get_symbol(), entry.order->get_order_id()});
            if (!process_order(entry.order, event_count, halt)) break;
        }
    }
    if (halt) return;

    {
        DEBUG_STAGE(stage_timer_, stop_check);
        check_pending_stops(rec.price, rec.price, rec.price, rec.timestamp, event_count, halt);
    }
    if (halt) return;

    auto tick_ptr = acquire_pooled(tick_pool_,te);
    log_event(te);
    {
        DEBUG_STAGE(stage_timer_, ring_publish);
        publish_event(tick_ptr);
    }
    if (!config_.is_threaded())
        analytics_.on_event(tick_ptr);
    else
        analytics_.on_mark(rec.symbol, rec.price);
    event_count++;

    if (evaluate_exits(rec.symbol, rec.price, rec.timestamp,
                       event_count, te.get_recv_ns()))
        return;

    std::optional<order_event> order_opt;
    {
        DEBUG_STAGE(stage_timer_, strategy);
        order_opt = strategy_->on_tick(te);
    }
    if (order_opt)
    {
        order_opt->set_recv_ns(te.get_recv_ns());
        if (!primary_strategy_name_.empty())
            order_opt->set_strategy_name(primary_strategy_name_);
        route_order(*order_opt, rec.timestamp, event_count, halt);
        if (!halt && strategy_)
            register_strategy_exit_intent(*strategy_, primary_strategy_name_, order_opt->get_order_id());
    }
    dispatch_extras_on_tick(te, rec.timestamp, event_count);
}

void engine::run_streaming(std::shared_ptr<DataBridge<bar_record>> bridge)
{
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    if (!config_.event_log_path.empty())
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path, config_.compress_log);

#ifdef HAS_QUESTDB
    questdb_begin();
#endif
    start_workers();
    pin_event_loop_thread();

    bridge->set_halt_flag(&halt_flag_);

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
            st.realized_pnl_fp4.store(
                static_cast<std::int64_t>(std::llround(analytics_.realized_pnl() * 1e4)),
                std::memory_order_relaxed);
            {
                const auto& positions = portfolio_.get_positions();
                double qty = 0.0, cost = 0.0;
                if (auto it = positions.find(rec.symbol); it != positions.end())
                {
                    qty  = it->second.qty;
                    cost = it->second.cost_basis;
                }
                const double unreal = qty * rec.close - cost;
                st.unrealized_pnl_fp4.store(
                    static_cast<std::int64_t>(std::llround(unreal * 1e4)),
                    std::memory_order_relaxed);
                st.position_qty_fp8.store(
                    static_cast<std::int64_t>(std::llround(qty * 1e8)),
                    std::memory_order_relaxed);
            }
            // Sign-flip so "%+.2f%%" renders "-15.50%" not "+15.50%".
            st.drawdown_fp4.store(
                -static_cast<std::int64_t>(std::llround(analytics_.max_drawdown_pct() * 1e2)),
                std::memory_order_relaxed);
            st.win_rate_bps.store(
                static_cast<std::uint32_t>(std::lround(analytics_.win_rate_pct() * 100.0)),
                std::memory_order_relaxed);
            // Simulated time so horizon compares correctly in replay.
            adverse_selection_.on_mark(rec.symbol, rec.close, timestamp);
            st.toxicity_bps_fp2.store(
                static_cast<std::int32_t>(std::lround(
                    adverse_selection_.mean_bps() * 100.0)),
                std::memory_order_relaxed);
            st.toxicity_samples.store(
                static_cast<std::uint32_t>(adverse_selection_.sample_count()),
                std::memory_order_relaxed);
            write_adapter_diagnostics(st);
        }

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
#ifdef HAS_QUESTDB
                maybe_questdb_tick();
#endif
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

    if (event_logger_) event_logger_->flush();
    stop_workers();
#ifdef HAS_QUESTDB
    questdb_end();
#endif
}

void engine::run_streaming(std::shared_ptr<DataBridge<tick_record>> bridge)
{
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    if (!config_.event_log_path.empty())
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path, config_.compress_log);

#ifdef HAS_QUESTDB
    questdb_begin();
#endif
    start_workers();
    pin_event_loop_thread();

    bridge->set_halt_flag(&halt_flag_);

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
            st.realized_pnl_fp4.store(
                static_cast<std::int64_t>(std::llround(analytics_.realized_pnl() * 1e4)),
                std::memory_order_relaxed);
            {
                const auto& positions = portfolio_.get_positions();
                double qty = 0.0, cost = 0.0;
                if (auto it = positions.find(rec.symbol); it != positions.end())
                {
                    qty  = it->second.qty;
                    cost = it->second.cost_basis;
                }
                const double unreal = qty * rec.price - cost;
                st.unrealized_pnl_fp4.store(
                    static_cast<std::int64_t>(std::llround(unreal * 1e4)),
                    std::memory_order_relaxed);
                st.position_qty_fp8.store(
                    static_cast<std::int64_t>(std::llround(qty * 1e8)),
                    std::memory_order_relaxed);
            }
            st.drawdown_fp4.store(
                -static_cast<std::int64_t>(std::llround(analytics_.max_drawdown_pct() * 1e2)),
                std::memory_order_relaxed);
            st.win_rate_bps.store(
                static_cast<std::uint32_t>(std::lround(analytics_.win_rate_pct() * 100.0)),
                std::memory_order_relaxed);
            adverse_selection_.on_mark(rec.symbol, rec.price, rec.timestamp);
            st.toxicity_bps_fp2.store(
                static_cast<std::int32_t>(std::lround(
                    adverse_selection_.mean_bps() * 100.0)),
                std::memory_order_relaxed);
            st.toxicity_samples.store(
                static_cast<std::uint32_t>(adverse_selection_.sample_count()),
                std::memory_order_relaxed);
            write_adapter_diagnostics(st);
        }

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
#ifdef HAS_QUESTDB
                maybe_questdb_tick();
#endif
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

    if (event_logger_) event_logger_->flush();
    stop_workers();
#ifdef HAS_QUESTDB
    questdb_end();
#endif
}

void engine::run_streaming(std::shared_ptr<DataBridge<provider::event>> bridge)
{
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    if (!config_.event_log_path.empty())
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path, config_.compress_log);

#ifdef HAS_QUESTDB
    questdb_begin();
#endif
    start_workers();
    pin_event_loop_thread();

    bridge->set_halt_flag(&halt_flag_);

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

    // current_event_ts tracks sim time of the last price-bearing event —
    // the clock used for AdverseSelectionTracker::on_mark so horizons stay
    // consistent in historical replay. L2 + status don't advance it.
    std::chrono::system_clock::time_point current_event_ts =
        std::chrono::system_clock::now();

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
                current_event_ts = timestamp;
                process_single_bar(rec, event_count, timestamp);
                record_count++;
            }
            else if constexpr (std::is_same_v<E, provider::tick>)
            {
                auto rec = provider::to_tick_record(e);
                current_event_ts = rec.timestamp;
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
                // (forward to queue models now centralized inside apply_l2_snapshot)
            }
            else if constexpr (std::is_same_v<E, provider::l2_update>)
            {
                tick_side ts = (e.side == 0) ? tick_side::bid : tick_side::ask;
                apply_l2_update(e.symbol, ts, e.price, e.new_quantity);
                // (forward to queue models now centralized inside apply_l2_update)
            }
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
            st.realized_pnl_fp4.store(
                static_cast<std::int64_t>(std::llround(analytics_.realized_pnl() * 1e4)),
                std::memory_order_relaxed);
            // Only mark once we've seen a price — L2/status may arrive first.
            if (last_mid_price_ > 0.0 && !last_mark_symbol_.empty())
            {
                const auto& positions = portfolio_.get_positions();
                double qty = 0.0, cost = 0.0;
                if (auto it = positions.find(last_mark_symbol_); it != positions.end())
                {
                    qty  = it->second.qty;
                    cost = it->second.cost_basis;
                }
                const double unreal = qty * last_mid_price_ - cost;
                st.unrealized_pnl_fp4.store(
                    static_cast<std::int64_t>(std::llround(unreal * 1e4)),
                    std::memory_order_relaxed);
                st.position_qty_fp8.store(
                    static_cast<std::int64_t>(std::llround(qty * 1e8)),
                    std::memory_order_relaxed);
            }
            st.drawdown_fp4.store(
                -static_cast<std::int64_t>(std::llround(analytics_.max_drawdown_pct() * 1e2)),
                std::memory_order_relaxed);
            st.win_rate_bps.store(
                static_cast<std::uint32_t>(std::lround(analytics_.win_rate_pct() * 100.0)),
                std::memory_order_relaxed);
            if (last_mid_price_ > 0.0 && !last_mark_symbol_.empty())
            {
                adverse_selection_.on_mark(last_mark_symbol_,
                                           last_mid_price_,
                                           current_event_ts);
            }
            st.toxicity_bps_fp2.store(
                static_cast<std::int32_t>(std::lround(
                    adverse_selection_.mean_bps() * 100.0)),
                std::memory_order_relaxed);
            st.toxicity_samples.store(
                static_cast<std::uint32_t>(adverse_selection_.sample_count()),
                std::memory_order_relaxed);
            write_adapter_diagnostics(st);
        }

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
#ifdef HAS_QUESTDB
                maybe_questdb_tick();
#endif
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

    if (event_logger_) event_logger_->flush();
    stop_workers();
#ifdef HAS_QUESTDB
    questdb_end();
#endif
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

    if (!config_.event_log_path.empty())
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path, config_.compress_log);

#ifdef HAS_DEBUG
    memory_sampler_.set_start(debug::memory_snapshot::capture());
#endif

#ifdef HAS_QUESTDB
    questdb_begin();
#endif
    start_workers();
    pin_event_loop_thread();

    const auto base_ts = (config_.seed != 0)
        ? std::chrono::system_clock::time_point(std::chrono::milliseconds(0))
        : std::chrono::system_clock::now();
    const auto n = data_handler_->db_data_symbol.size();
    analytics_.reserve_hint(n);
    const auto start = std::chrono::high_resolution_clock::now();
    if (config_.show_progress) {
        std::cout << "\rProgress: 0.000% | Trades executed: 0" << std::flush;
    }

    std::size_t event_count = 0;
    auto last_report_time = std::chrono::steady_clock::now();

    pending_stops_.clear();
    while (!pending_orders_.empty()) pending_orders_.pop();
    order_seq_ = 0;
    day_order_ids_.clear();

    bool halt_requested = false;

    // seed != 0 keeps legacy synthetic stepping (base + i ms) for golden
    // reproducibility. Normal runs read the CSV date and fall back to +1ms
    // on parse failure / regression — never silent reordering.
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
            // Re-center the synthetic book at the open before draining:
            // next-bar-open fills must walk depth priced at the open, not
            // at the previous close (visible on gap bars).
            if (!pending_orders_.empty() &&
                pending_orders_.top().order->get_earliest_eligible_ts() <= sim_time)
            {
                auto ob_open = orderbook_registry_.get_or_create(symbol);
                if (!mm_worker_ &&
                    !l2_seeded_symbols_.count(symbol))
                {
                    auto mm_trades = market_maker_.replenish(
                        ob_open, last_mid_price_, /*update_history=*/false);
                    deliver_mm_book_trades(symbol, mm_trades, sim_time,
                                           event_count, halt_requested);
                }
            }
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
            check_pending_stops(mkt.get_open(), mkt.get_high(), mkt.get_low(), sim_time, event_count, halt_requested);
        }
        sweep_resting_limits(symbol, mkt.get_low(), mkt.get_high(),
                             sim_time, event_count, halt_requested);

        {
            DEBUG_STAGE(stage_timer_, mm_replenish);
            auto ob = orderbook_registry_.get_or_create(symbol);
            if (!mm_worker_ &&
                !l2_seeded_symbols_.count(symbol))
            {
                auto mm_trades = market_maker_.replenish(ob, last_mid_price_);
                deliver_mm_book_trades(symbol, mm_trades, sim_time,
                                       event_count, halt_requested);
            }
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
                    auto mm_ob_order = mm_ob->create_order(
                        ob_order_type::good_till_cancel, mm_order.get_order_id(),
                        mm_side, Price::from_double(mm_order.get_price()),
                        static_cast<quantity>(std::round(mm_order.get_quantity() * 1e8)));
                    auto mm_trades = mm_ob->add_order(mm_ob_order);
                    deliver_mm_book_trades(mm_order.get_symbol(), mm_trades,
                                           sim_time, event_count, halt_requested);
                }
            }
        }

        auto mkt_ptr = acquire_pooled(market_pool_,mkt);
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
        else
            analytics_.on_mark(symbol, mkt.get_close());
        event_count++;

        if (evaluate_exits(symbol,
                           mkt.get_open(), mkt.get_low(), mkt.get_high(), mkt.get_close(),
                           sim_time, event_count, mkt.get_recv_ns()))
            break;

        if (order_opt)
        {
            order_opt->set_recv_ns(mkt.get_recv_ns());
            if (!primary_strategy_name_.empty())
                order_opt->set_strategy_name(primary_strategy_name_);
            route_order(*order_opt, sim_time, event_count, halt_requested);
            if (!halt_requested && strategy_)
                register_strategy_exit_intent(*strategy_, primary_strategy_name_, order_opt->get_order_id());
        }
        dispatch_extras_on_market(mkt, sim_time, event_count);

        {
            auto now_report = std::chrono::steady_clock::now();
            if ((i + 1) == n || now_report - last_report_time >= std::chrono::milliseconds(200))
            {
                const double progress = ((i + 1) * 100.0) / static_cast<double>(n);
                if (config_.show_progress) {
                    std::cout << "\rProgress: " << std::fixed << std::setprecision(3) << progress
                              << "% | Trades executed: " << portfolio_.get_total_trades()
                              << std::flush;
                }
                last_report_time = now_report;
#ifdef HAS_QUESTDB
                maybe_questdb_tick();
#endif
            }
        }

        write_checkpoint_if_due(event_count);
    }

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

    if (config_.show_progress) {
        std::cout << std::endl;
        std::cout << "Trades executed: " << portfolio_.get_total_trades()
                  << " in " << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

        double throughput = static_cast<double>(event_count) / (elapsed_ms / 1000.0);
        std::cout << "Event throughput: " << throughput << " events/second" << std::endl;
    }

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
#ifdef HAS_QUESTDB
    questdb_end();
#endif

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

    pending_stops_.clear();
    while (!pending_orders_.empty()) pending_orders_.pop();
    order_seq_ = 0;
    day_order_ids_.clear();

#ifdef HAS_DEBUG
    memory_sampler_.set_start(debug::memory_snapshot::capture());
#endif

#ifdef HAS_QUESTDB
    questdb_begin();
#endif
    start_workers();
    pin_event_loop_thread();

    const auto& ticks = data_handler_->tick_data;
    const auto n = ticks.size();
    const auto start = std::chrono::high_resolution_clock::now();

    if (config_.show_progress) {
        std::cout << "\rProgress: 0.000% | Trades executed: 0" << std::flush;
    }

    std::size_t event_count = 0;
    bool halt_requested = false;
    auto last_report_time = std::chrono::steady_clock::now();

    BarAggregator bar_agg(std::chrono::seconds(1), [&](const market_event& bar)
    {
        int64_t bar_recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        auto bar_ptr = acquire_pooled(market_pool_,bar);
        bar_ptr->set_recv_ns(bar_recv_ns);
        auto order_opt = strategy_->on_market(bar);
        publish_event(bar_ptr);
        if (!config_.is_threaded())
            analytics_.on_event(bar_ptr);
        else
            analytics_.on_mark(bar.get_symbol(), bar.get_close());

        if (evaluate_exits(bar.get_symbol(),
                           bar.get_open(), bar.get_low(), bar.get_high(), bar.get_close(),
                           bar.get_timestamp(), event_count, bar_recv_ns))
            return;

        if (order_opt)
        {
            order_opt->set_recv_ns(bar_recv_ns);
            if (!primary_strategy_name_.empty())
                order_opt->set_strategy_name(primary_strategy_name_);
            route_order(*order_opt, bar.get_timestamp(), event_count, halt_requested);
            if (!halt_requested && strategy_)
                register_strategy_exit_intent(*strategy_, primary_strategy_name_, order_opt->get_order_id());
        }
        dispatch_extras_on_market(bar, bar.get_timestamp(), event_count);
    });

    for (std::size_t i = 0; i < n && !halt_requested
             && !halt_flag_.load(std::memory_order_acquire)
             && !worker_failed_.load(std::memory_order_acquire); ++i)
    {
        const auto& tick = ticks[i];

        last_mid_price_ = tick.price;

        {
            DEBUG_STAGE(stage_timer_, mm_replenish);
            auto ob = orderbook_registry_.get_or_create(tick.symbol);
            if (!l2_seeded_symbols_.count(tick.symbol))
            {
                auto mm_trades = market_maker_.replenish(ob, last_mid_price_);
                deliver_mm_book_trades(tick.symbol, mm_trades, tick.timestamp,
                                       event_count, halt_requested);
            }
        }

        {
            DEBUG_STAGE(stage_timer_, pending_drain);
            while (!pending_orders_.empty() &&
                   pending_orders_.top().order->get_earliest_eligible_ts() <= tick.timestamp)
            {
                auto entry = pending_orders_.top();
                pending_orders_.pop();
                if (entry.order->get_tif() == time_in_force::day)
                    day_order_ids_.push_back({entry.order->get_symbol(), entry.order->get_order_id()});
                if (!process_order(entry.order, event_count, halt_requested)) break;
            }
        }
        if (halt_requested) break;

        {
            DEBUG_STAGE(stage_timer_, stop_check);
            check_pending_stops(tick.price, tick.price, tick.price, tick.timestamp, event_count, halt_requested);
        }
        if (halt_requested) break;

        tick_side ts = tick_side::unknown;
        if (tick.side == data_tick_side::bid) ts = tick_side::bid;
        else if (tick.side == data_tick_side::ask) ts = tick_side::ask;

        tick_event te(tick.timestamp, tick.symbol, tick.price, tick.quantity, ts);
        te.set_recv_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        auto tick_ptr = acquire_pooled(tick_pool_,te);
        log_event(te);
        {
            DEBUG_STAGE(stage_timer_, ring_publish);
            publish_event(tick_ptr);
        }
        if (!config_.is_threaded())
            analytics_.on_event(tick_ptr);
        else
            analytics_.on_mark(tick.symbol, tick.price);
        event_count++;

        if (evaluate_exits(tick.symbol, tick.price, tick.timestamp,
                           event_count, te.get_recv_ns()))
            break;

        std::optional<order_event> order_opt;
        {
            DEBUG_STAGE(stage_timer_, strategy);
            order_opt = strategy_->on_tick(te);
        }
        if (order_opt)
        {
            order_opt->set_recv_ns(te.get_recv_ns());
            if (!primary_strategy_name_.empty())
                order_opt->set_strategy_name(primary_strategy_name_);
            route_order(*order_opt, tick.timestamp, event_count, halt_requested);
            if (!halt_requested && strategy_)
                register_strategy_exit_intent(*strategy_, primary_strategy_name_, order_opt->get_order_id());
        }
        dispatch_extras_on_tick(te, tick.timestamp, event_count);
        if (halt_requested) break;

        bar_agg.on_tick(tick.symbol, tick.price, tick.quantity, tick.timestamp);

        {
            auto now_report = std::chrono::steady_clock::now();
            if ((i + 1) == n || now_report - last_report_time >= std::chrono::milliseconds(200))
            {
                const double progress = ((i + 1) * 100.0) / static_cast<double>(n);
                if (config_.show_progress) {
                    std::cout << "\rProgress: " << std::fixed << std::setprecision(3) << progress
                              << "% | Trades executed: " << portfolio_.get_total_trades()
                              << std::flush;
                }
                last_report_time = now_report;
#ifdef HAS_QUESTDB
                maybe_questdb_tick();
#endif
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

    if (config_.show_progress) {
        std::cout << std::endl;
        std::cout << "Trades executed: " << portfolio_.get_total_trades()
                  << " in " << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

        double throughput = static_cast<double>(event_count) / (elapsed_ms / 1000.0);
        std::cout << "Event throughput: " << throughput << " events/second" << std::endl;
    }

    if (event_logger_) event_logger_->flush();
    stop_workers();
#ifdef HAS_QUESTDB
    questdb_end();
#endif

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

void engine::run_replay(const std::string& log_path,
                        int64_t replay_from_us,
                        int64_t replay_to_us)
{
    EventReplayer replayer(log_path, replay_from_us, replay_to_us);

#ifdef HAS_QUESTDB
    questdb_begin();
#endif
    start_workers();
    pin_event_loop_thread();

    // Replay must go through route_order+process_order so instrument spec,
    // exec delay, latency, stops, and day-order behavior stay identical to
    // the original run (not a thinner inline path).
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

            // Re-center the synthetic book at the open before draining
            // (next-bar-open fills walk open-priced depth; see bar loop).
            if (!pending_orders_.empty() &&
                pending_orders_.top().order->get_earliest_eligible_ts() <= sim_time)
            {
                auto ob_open = orderbook_registry_.get_or_create(mkt.get_symbol());
                if (!l2_seeded_symbols_.count(mkt.get_symbol()))
                {
                    auto mm_trades = market_maker_.replenish(
                        ob_open, last_mid_price_, /*update_history=*/false);
                    deliver_mm_book_trades(mkt.get_symbol(), mm_trades, sim_time,
                                           event_count, halt_requested);
                }
            }
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

            check_pending_stops(mkt.get_open(), mkt.get_high(), mkt.get_low(), sim_time,
                                event_count, halt_requested);
            if (halt_requested) break;
            sweep_resting_limits(mkt.get_symbol(), mkt.get_low(), mkt.get_high(),
                                 sim_time, event_count, halt_requested);
            if (halt_requested) break;

            auto ob = orderbook_registry_.get_or_create(mkt.get_symbol());
            if (!l2_seeded_symbols_.count(mkt.get_symbol()))
            {
                auto mm_trades = market_maker_.replenish(ob, last_mid_price_);
                deliver_mm_book_trades(mkt.get_symbol(), mm_trades, sim_time,
                                       event_count, halt_requested);
            }

            auto order_opt = strategy_->on_market(mkt);
            publish_event(ev);
            if (!config_.is_threaded())
                analytics_.on_event(ev);
            else
                analytics_.on_mark(mkt.get_symbol(), mkt.get_close());

            if (evaluate_exits(mkt.get_symbol(),
                               mkt.get_open(), mkt.get_low(), mkt.get_high(), mkt.get_close(),
                               sim_time, event_count, mkt.get_recv_ns()))
                break;

            if (order_opt)
            {
                if (!primary_strategy_name_.empty())
                    order_opt->set_strategy_name(primary_strategy_name_);
                order_opt->set_recv_ns(mkt.get_recv_ns());
                route_order(*order_opt, sim_time, event_count, halt_requested);
                if (!halt_requested && strategy_)
                    register_strategy_exit_intent(*strategy_, primary_strategy_name_, order_opt->get_order_id());
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

            check_pending_stops(te.get_price(), te.get_price(), te.get_price(), sim_time,
                                event_count, halt_requested);
            if (halt_requested) break;

            publish_event(ev);
            if (!config_.is_threaded())
                analytics_.on_event(ev);
            else
                analytics_.on_mark(te.get_symbol(), te.get_price());

            if (evaluate_exits(te.get_symbol(), te.get_price(), sim_time,
                               event_count, te.get_recv_ns()))
                break;

            auto order_opt = strategy_->on_tick(te);
            if (order_opt)
            {
                if (!primary_strategy_name_.empty())
                    order_opt->set_strategy_name(primary_strategy_name_);
                order_opt->set_recv_ns(te.get_recv_ns());
                route_order(*order_opt, sim_time, event_count, halt_requested);
                if (!halt_requested && strategy_)
                    register_strategy_exit_intent(*strategy_, primary_strategy_name_, order_opt->get_order_id());
            }
            dispatch_extras_on_tick(te, sim_time, event_count);
            break;
        }
        default:
            // Non-price events are re-published for observers but not
            // re-executed — replay regenerates orders from the strategy.
            publish_event(ev);
            if (!config_.is_threaded())
                analytics_.on_event(ev);
            break;
        }

        event_count++;
    }

    // Drain so queued state doesn't leak across replays.
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
#ifdef HAS_QUESTDB
    questdb_end();
#endif
}
