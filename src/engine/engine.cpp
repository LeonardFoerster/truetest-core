#include "engine.h"
#include "checkpoint.h"
#ifdef HAS_QUESTDB
#include "data/questdb/run_tag.h"
#endif
#include "data/data_handler.h"
#include "data/date_parse.h"
#include "execution/portfolio.h"
#include "execution/latency_model.h"
#include "execution/async_support.h"
#include "execution/fill_parser.h"
#include "execution/fee_model.h"
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
// LIVE-SAFETY SURFACE — Phase 1 freeze (see prod.md, 02-prerequisites.md)
// Any edit requires explicit two-person CCB review + 4 h
// mainnet shadow run on engine_shadow before merge.
// Commit message MUST contain: LIVE_SAFETY_CCB_APPROVED
// Run: ./scripts/check-live-safety-freeze.sh after changes.
// Files in this set: tt_target.h, engine.{h,cpp}, all
// *kill_switch*, *dead_mans_switch*, *reconciler* under
// providers/binance/, risk/*, ExecutionBridge, live_safety.h
//
// Engine decomposition (see core/docs/internal/engine-decomposition.md + ~/.grok/skills/engine-decomposition/SKILL.md):
// Phase 2 prep in progress. Cold-path extractions (dashboard, scheduler, workers, run skeleton)
// planned in subsequent waves. All changes must preserve zero-alloc hot path,
// identical behavior for MC reuse / backtest / shadow / live, and single
// IOrderAuditSink + ExecutionRouter seams. No direct questdb recording or
// ad-hoc adapter bypasses allowed.
// ENGINE_LOC_WAIVER: (historical; Wave 1 extraction complete, engine.cpp now well under limit)
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
    // FR-zero-fee-default: always echo fee model in report JSON (never imply fees applied).
    if (!config_.fee_model)
        analytics_.set_fee_model("zero");
    else if (dynamic_cast<const FixedFeeModel*>(config_.fee_model.get()))
        analytics_.set_fee_model("fixed");
    else if (dynamic_cast<const TieredFeeModel*>(config_.fee_model.get()))
        analytics_.set_fee_model("tiered");
    else
        analytics_.set_fee_model("custom");
    // Checkpoint mgr early (used by restore path in ctor).
    checkpoint_mgr_ = std::make_unique<CheckpointManager>(config_);

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

    // Soft post-fill continue is backtest-research only. Never fail-open on
    // live/shadow even if a caller left the flag true (default is true).
    if (config_.mode != engine_mode::backtest)
        config_.risk_soft_portfolio_limits = false;

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
        auto armed_for_pub = callbacks_armed_flag_;
        config_.provider->set_event_publisher(
            [this, armed_for_pub](std::shared_ptr<event> ev) {
                if (!armed_for_pub || !armed_for_pub->load(std::memory_order_acquire)) return;
                publish_event(ev);
            });

        auto armed_for_funding = callbacks_armed_flag_;
        config_.provider->set_funding_event_factory(
            [this, armed_for_funding](std::chrono::system_clock::time_point ts,
                   const std::string& symbol,
                   double cash_delta,
                   const std::string& reason) {
                if (!armed_for_funding || !armed_for_funding->load(std::memory_order_acquire))
                    return std::shared_ptr<funding_event>{};
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
            auto armed_for_watchdog = callbacks_armed_flag_;
            worker_watchdog_->set_halt_callback(
                [this, armed_for_watchdog](std::string_view source, std::int64_t age_ms) {
                    if (!armed_for_watchdog || !armed_for_watchdog->load(std::memory_order_acquire)) return;
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
            if (auto* cap = adapter->get_async_support())
            {
                auto armed_for_unknown = callbacks_armed_flag_;
                cap->set_unknown_fill_handler(
                    [this, armed_for_unknown](const parsed_exec& msg, std::uint64_t fill_id)
                        -> std::optional<synth_result>
                    {
                        if (!armed_for_unknown || !armed_for_unknown->load(std::memory_order_acquire))
                            return std::nullopt;

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
                        return synth_result{
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
            auto armed_for_halt = callbacks_armed_flag_;
            config_.provider->set_halt_callback(
                [this, armed_for_halt](std::string_view reason) {
                    if (!armed_for_halt || !armed_for_halt->load(std::memory_order_acquire)) return;
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

    // Wire new seams (PR-03, behind existing activation, minimal).
    // See core/docs/internal/engine-decomposition.md Phase 2 (E-21) + engine-decomposition skill:
    // All recording MUST go exclusively through audit_sink_ (IOrderAuditSink).
    // No raw questdb decision sites for data capture. Activation only here.
    // Router owns adapter resolution, submit/poll, L2, advance. No ad-hoc bypass.
    audit_sink_ = std::make_unique<NoopOrderAuditSink>();
#ifdef HAS_QUESTDB
    if (config_.persist_enabled) {
        questdb_begin();  // activation (including sink swap to real QuestdbOrderAuditSink on success) lives inside
    }
#endif

    // Router wiring (adapters map passed by ref so resolve populates the original execution_adapters_ for iterator compat).
    // See core/docs/internal/engine-decomposition.md (execution router extraction) and engine-decomposition/SKILL.md.
    router_ = std::make_unique<ExecutionRouter>(
        orderbook_registry_,
        config_,
        l2_seeded_symbols_,
        config_.provider.get(),
        pending_cancels_,
        order_meta_,
        execution_adapters_
    );

    // InstrumentSpecCache wiring (PR final). Refs to overrides/provider (cold).
    instrument_spec_cache_ = std::make_unique<InstrumentSpecCache>(
        config_.instrument_overrides, config_.provider.get());

    // Wave 1: create the dashboard builder (cold path extraction).
    // Pass all needed non-owning refs for identical snapshot data + caches.
    dashboard_builder_ = std::make_unique<DashboardSnapshotBuilder>(
        portfolio_,
        analytics_,
        adverse_selection_,
        exit_manager_,
        halt_flag_,
        config_,
        last_mid_price_,
        last_mark_symbol_,
        last_mark_prices_,
        last_mark_prices_mu_,
        orderbook_registry_,
        execution_adapters_,
        *audit_sink_,
        l2_seeded_symbols_,
        market_pool_,
        order_pool_,
        fill_pool_,
        tick_pool_,
        l2_update_pool_,
        l2_snapshot_pool_,
        rejection_pool_,
        cancel_pool_,
        amend_pool_,
        funding_pool_,
        control_block_pool_,
        logging_ring_,
        risk_ring_,
        stats_ring_,
        observer_ring_,
        risk_stats_ring_,
        mm_ring_,
#ifdef HAS_DEBUG
        DebugSamplers{&stage_timer_, &memory_sampler_}
#else
        DebugSamplers{}
#endif
    );

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
    if (checkpoint_mgr_) checkpoint_mgr_->write_if_due(portfolio_, event_count);
}

void engine::restore_from_checkpoint()
{
    if (checkpoint_mgr_) checkpoint_mgr_->restore(portfolio_);
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
    // MarketSeries read/write API (docs/internal/data-pipeline.md#D-02) — no public SoA fields.
    data_handler_->set_all_bar_symbols(new_symbol);

    strategy_->set_position_open(new_symbol, false);
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
    // Opportunistic drain of worker-thread deferred pool returns.
    // This reduces apparent exhaustion and mutex contention on the next
    // acquire when workers release objects between engine acquires.
    // Zero-alloc and safe on hot path (cheap atomic + occasional mutex work).
    drain_object_pool_returns();

    if (dashboard_builder_) dashboard_builder_->refresh_if_due();

    // Phase 2: funding settlements update the primary portfolio cash immediately
    // (advisory for now; later will also feed risk_snapshot / RiskManager).
    if (ev && ev->get_type() == event_type::funding) {
        if (auto* fe = dynamic_cast<funding_event*>(ev.get())) {
            portfolio_.on_funding(*fe);
            // Unconditional via sink seam (run_tag supplied by sink; "" when no persistence).
            // No direct questdb_store_ access. Zero-alloc public seam.
            audit_sink_->record_funding(*fe, audit_sink_ ? audit_sink_->run_tag() : "");
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
    if (dashboard_builder_) return dashboard_builder_->snapshot_dashboard(out);
    return false;
}

void engine::request_dashboard_refresh()
{
    if (dashboard_builder_) dashboard_builder_->request_dashboard_refresh();
}

// Dashboard methods moved to DashboardSnapshotBuilder (Wave 1).
// Old implementations removed; calls updated to delegate via dashboard_builder_.

// (stray build_dashboard_view body excised)

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

// Wave 2 skeleton helpers (setup/teardown, pending drain, paper tape, marks)
// live in engine_pending.cpp to keep freeze-surface engine.cpp from sprawling.

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
    // Worker/ring orchestration. See core/docs/internal/engine-decomposition.md Wave 4 (E-60) + engine-decomposition.
    // Will be delegated to WorkerOrchestrator (rings, pinning, start/stop, drops).
    halt_flag_.store(false, std::memory_order_release);
    provider_callbacks_armed_.store(true, std::memory_order_release);
    if (callbacks_armed_flag_) callbacks_armed_flag_->store(true, std::memory_order_release);
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

engine::~engine()
{
    // Ensure workers are joined and provider resources (incl. any
    // lingering transport threads and callbacks) are torn down before
    // our members (pools, rings, exit_manager, etc.) are destroyed.
    stop_workers();

    // Additional explicit clear of provider callbacks in case a code
    // path constructed the engine but never ran a full stop (e.g. early
    // exception after wiring but before run()).
    provider_callbacks_armed_.store(false, std::memory_order_release);
    if (callbacks_armed_flag_) callbacks_armed_flag_->store(false, std::memory_order_release);
    revoke_provider_callbacks();

#ifdef NDEBUG
    // In release, keep silent. In debug we want the checks below.
#else
    // Phase 3: assert that pooled objects have all been released by
    // the time the engine dies. Violations indicate lifetime bugs
    // (shared_ptrs escaping via rings, late callbacks, etc.).
    // These are debug-only; release builds do not pay for them.
    auto check_pool = [](const char* name, auto& pool) {
        const auto u = pool.in_use();
        if (u != 0) {
            std::fprintf(stderr,
                "[engine dtor] WARNING: %s still has %zu objects in_use()\n",
                name, u);
        }
    };
    check_pool("market_pool", market_pool_);
    check_pool("order_pool", order_pool_);
    check_pool("fill_pool", fill_pool_);
    check_pool("tick_pool", tick_pool_);
    check_pool("l2_update_pool", l2_update_pool_);
    check_pool("l2_snapshot_pool", l2_snapshot_pool_);
    check_pool("rejection_pool", rejection_pool_);
    check_pool("cancel_pool", cancel_pool_);
    check_pool("amend_pool", amend_pool_);
    check_pool("funding_pool", funding_pool_);
    // control_block_pool_ has similar accounting
    const auto cb_in_use = control_block_pool_.in_use();
    if (cb_in_use != 0) {
        std::fprintf(stderr,
            "[engine dtor] WARNING: control_block_pool still has %zu in_use()\n",
            cb_in_use);
    }
#endif
}

void engine::revoke_provider_callbacks()
{
    if (!config_.provider) return;

    if (auto adapter = config_.provider->get_execution_adapter())
    {
        if (auto* cap = adapter->get_async_support())
            cap->clear_unknown_fill_handler();
    }
    if (config_.mode == engine_mode::live)
    {
        try { config_.provider->close(); } catch (...) {}
    }
    config_.provider->set_halt_callback([](std::string_view){});
    config_.provider->set_event_publisher([](std::shared_ptr<event>){});
    config_.provider->set_funding_event_factory(
        [](auto, const auto&, double, const auto&) -> std::shared_ptr<funding_event> {
            return {};
        });
}

void engine::stop_workers()
{
    // Shutdown sequence (improved for memory safety per 2026-07-18 check):
    // 1. Disarm armed flags (so future callback bodies early-return).
    // 2. Drain deferred.
    // 3. QuestDB last flush.
    // 4. Stop watchdog.
    // 5. revoke_provider_callbacks() (clears + conditional close).
    // 6. stop workers (join threads).
    // 7. drain rings.
    // 8. (live) kill switch flatten.
    // This order reduces the live window for in-flight [this] bodies vs dtor.
    // Future: could extract ShutdownCoordinator for the order.
    provider_callbacks_armed_.store(false, std::memory_order_release);
    if (callbacks_armed_flag_) callbacks_armed_flag_->store(false, std::memory_order_release);

    // Early drain so worker releases are visible before we revoke callbacks
    // and join. Helps ensure in_use() is low before pools/rings are torn down.
    drain_object_pool_returns();

    // QuestDB: give pending ILP enqueues (from audit sink on any thread)
    // a last flush opportunity before we tear down workers/rings that may
    // still produce final records. Best-effort; full wait would require
    // dedicated flush thread + shutdown latch in IlpWriter (future).
#ifdef HAS_QUESTDB
    if (questdb_store_) {
        try { questdb_store_->flush(); } catch (...) {}
    }
#endif

    // Stop watchdog early (its poll thread holds a callback into us).
    // Do this right after disarm + before joins and heavy teardown.
    if (worker_watchdog_) worker_watchdog_->stop();

    // Centralized revocation (includes close for live providers, no-op clears
    // for all installed [this] callbacks). This happens after disarm so that
    // any already-dispatched in-flight bodies that passed the armed check
    // see no-op handlers if they re-enter, and transport threads are stopped.
    revoke_provider_callbacks();

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

    // Workers joined — safe to stamp research counters onto export analytics.
    fold_research_counters_into_export_analytics();

    // Best-effort drain of rings after workers are joined. Any remaining
    // references held by external ring shared_ptr copies (tests, UI, etc.)
    // may still drop later; their deleters are now protected by the
    // pool alive_ guards + armed callback guards.
    auto drain_ring = [](const std::shared_ptr<EventRing>& r) {
        if (!r) return;
        event_pointer ev;
        while (r->try_pop(ev)) {}
    };
    drain_ring(logging_ring_);
    drain_ring(risk_ring_);
    drain_ring(stats_ring_);
    drain_ring(observer_ring_);
    drain_ring(risk_stats_ring_);
    drain_ring(mm_ring_);

    if (mm_order_ring_)
    {
        event_pointer ev; // different ring type, keep original loop
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
    if (data_handler_ && data_handler_->has_bar_data())
        scfg.symbol = data_handler_->first_symbol();
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
        // Single canonical place that decides the concrete sink (ctor starts with Noop).
        // No raw if(active && store) remains in caller paths.
        audit_sink_ = std::make_unique<QuestdbOrderAuditSink>(questdb_store_, &questdb_active_);
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
    double final_equity;
    {
        std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
        final_equity = portfolio_.get_equity(
            last_mark_prices_, last_mid_price_.load(std::memory_order_relaxed));
    }
    const std::size_t rejs = audit_sink_ ? audit_sink_->total_rejections() : 0;
    // Prefer delegating through the seam when available (single place for persistence finalization).
    // See core/docs/internal/engine-decomposition.md Phase 2 (E-21) + engine-decomposition skill "QuestDB Isolation".
    // The else branch below is legacy fallback only (should be unreachable when questdb_active_).
    // All recording paths use audit_sink_; finalize should too.
    if (audit_sink_)
    {
        audit_sink_->finalize_run(final_equity,
                                  report.total_orders,
                                  report.total_fills,
                                  rejs,
                                  report.max_drawdown,
                                  report.sharpe_ratio,
                                  report.sortino_ratio,
                                  report.profit_factor,
                                  report.win_rate,
                                  report.calmar_ratio,
                                  report.total_trades,
                                  report.winning_trades);
    }
    // Note: legacy direct questdb_store_ finalize path removed in Phase 2 prep
    // (core/docs/internal/engine-decomposition.md#E-21) to enforce single IOrderAuditSink seam.
    // Activation in questdb_begin always sets a real sink when store is active.
    questdb_active_ = false;
}
#endif

void engine::maybe_questdb_tick()
{
#ifdef HAS_QUESTDB
    if (!questdb_active_ || !questdb_store_) return;
    auto now = std::chrono::steady_clock::now();
    if (now - last_questdb_flush_ >= config_.questdb_flush_cadence) {
        questdb_store_->tick();
        last_questdb_flush_ = now;
    }
#endif
}

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
    // Use the IExecutionAdapter virtuals (implemented by TradeTapeShadowAdapter
    // and others); no concrete cast required.
    if (config_.mode == engine_mode::shadow && config_.provider)
    {
        auto exec = config_.provider->get_execution_adapter();
        if (exec)
        {
            const auto submitted = exec->queue_submitted_with_queue();
            if (submitted > 0)
            {
                std::cout << "  Queue model (shadow):\n"
                          << "    Submitted with queue ahead: " << submitted << "\n"
                          << "    Filled after queue drained: " << exec->queue_filled_after_drain() << "\n"
                          << "    Still queue-blocked at EOS: " << exec->queue_blocked_at_eos() << "\n";
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
            double last_price = (last_mid_price_.load(std::memory_order_relaxed) > 0.0) ? last_mid_price_.load(std::memory_order_relaxed) : 0.0;

            double sim_equity, exch_equity;
            {
                std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
                sim_equity  = portfolio_.get_equity(last_mark_prices_, last_price);
                exch_equity = exch->get_equity(last_mark_prices_, last_price);
            }
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

void engine::set_data_rows_rejected(std::size_t n)
{
    // Stored engine-local and on analytics_; folded into worker export analytics
    // at stop_workers (after join) so threaded get_analytics() is honest.
    data_rows_rejected_ = n;
    analytics_.set_data_rows_rejected(n);
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
    // Reset for MC object reuse. See core/docs/internal/engine-decomposition.md (Phase 0 notes + future waves).
    // State owned by future extracted collaborators will be cleared via their clear/reset hooks.
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
    last_mid_price_.store(0.0, std::memory_order_release);
    last_mark_symbol_.clear();
    prepare_mark_prices_for_run();
    soft_post_fill_breaches_ = 0;
    data_rows_rejected_ = 0;

    // Clear orderbook registry (L2 state from previous trial)
    orderbook_registry_.clear();

    // Adapters / pending DAY ids / execution_adapters_ are NOT fully cleared
    // here. MC still uses a fresh engine per trial (see MonteCarloController);
    // do not claim full in-place reuse readiness until those are reset too.

    // Reset market maker and adverse selection trackers
    market_maker_.reset();
    adverse_selection_.reset();

    // Reset per-symbol caches that can leak state between trials
    if (instrument_spec_cache_) instrument_spec_cache_->clear();
    l2_seeded_symbols_.clear();

    if (dashboard_builder_) dashboard_builder_->clear_for_mc_reset();

    // Reset tick aggregator (prevents partial bar leakage between trials)
    if (tick_aggregator_)
    {
        tick_aggregator_->reset();
    }

    // Clear UI/dashboard caches (harmless and cheap for headless MC runs)
    if (dashboard_builder_) {
        // the builder owns them now; for reset we can request refresh or clear via public if exposed, but for now force reinit on next
        // since state moved, the builder will handle in its own reset if we add later.
    }

    // Phase 4 MC reuse hardening: clear order_meta_ for clean per-trial isolation
    // (opener/strategy attribution must not leak between independent trials).
    order_meta_.clear();
    pending_cancels_.clear();

    // Clear shadow_tracker for per-trial isolation (divergence tracking not
    // relevant across MC trials; see MC controller comment).
    if (shadow_tracker_) shadow_tracker_->reset();

    // Opportunistic drain for clean in_use() counters for the next trial.
    // Does not solve escaped shared_ptrs from previous trial (those are a
    // contract violation for the MC controller / test harness).
    drain_object_pool_returns();

    // Re-arm for safety on reused engine objects (callbacks are typically
    // not live during MC, but keep the flag consistent).
    provider_callbacks_armed_.store(true, std::memory_order_release);
    if (callbacks_armed_flag_) callbacks_armed_flag_->store(true, std::memory_order_release);

    // Re-arm pool alive guards for MC reuse of the engine instance.
    // IMPORTANT: Callers (MC controller, tests) must ensure no shared_ptr<Event>
    // or pooled objects from the *prior epoch/trial* are still held by external
    // observers when reset_for_next_trial is called. Our Returner epoch check
    // makes late drops from old epochs safe (they leak instead of UAF), but
    // outstanding live objects from prior trial violate the reuse contract and
    // may observe stale state in rings/portfolio until drained.
    market_pool_.rearm_for_reuse();
    order_pool_.rearm_for_reuse();
    fill_pool_.rearm_for_reuse();
    tick_pool_.rearm_for_reuse();
    l2_update_pool_.rearm_for_reuse();
    l2_snapshot_pool_.rearm_for_reuse();
    rejection_pool_.rearm_for_reuse();
    cancel_pool_.rearm_for_reuse();
    amend_pool_.rearm_for_reuse();
    funding_pool_.rearm_for_reuse();

    // Phase 4 MC safety: if any pool still reports in_use() > 0 here, it means
    // there were escaped shared_ptrs from the prior trial. Our late-drop
    // logic will leak them on drop rather than UAF, but this indicates a
    // contract violation by the caller (MC harness or test). Log in debug.
#ifndef NDEBUG
    auto check_in_use = [](const char* name, auto& pool) {
        auto u = pool.in_use();
        if (u != 0) {
            std::fprintf(stderr,
                "[reset_for_next_trial] WARNING: %s still has %zu in_use() "
                "(escaped objects from prior epoch?)\n", name, u);
        }
    };
    check_in_use("market", market_pool_);
    check_in_use("order", order_pool_);
    check_in_use("fill", fill_pool_);
    // ... (others similar; abbreviated)
#endif

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
    // S3: process-wide halt is terminal for ALL submit call sites, including
    // check_pending_stops / pending drains that bypass route_order.
    if (halt_flag_.load(std::memory_order_acquire))
    {
        halt_requested = true;
        return false;
    }

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
            auto vd = risk_check_->evaluate(*o, portfolio_, last_mid_price_.load(std::memory_order_relaxed));
            if (!vd.allow)
            {
                const std::string reason_str =
                    "venue risk check refused: " + vd.reason;
                auto rej = acquire_pooled(rejection_pool_,
                    o->get_timestamp(), o->get_symbol(),
                    o->get_order_id(), reason_str);
                log_event(*rej);
                publish_event(rej);
                // Migrated to sink (PR-04)
                audit_sink_->record_order_submitted(*o, "rejected");
                // Use stack buffer for audit detail to avoid string temp on this path (pooled still needs string)
                char venue_reason[128];
                std::snprintf(venue_reason, sizeof(venue_reason), "venue risk check refused: %s", vd.reason.c_str());
                audit_sink_->record_rejection(*o, "venue_risk_reject", venue_reason);
                order_tracker_.set_status(o->get_order_id(),
                                          order_status::rejected);
                if (dashboard_builder_) dashboard_builder_->erase_open_order(o->get_order_id());
                // Reject, not halt — engine continues. The cap describes
                // what this operator considers prudent, not a market-wide
                // risk-of-ruin condition that should stop everything.
                return true;
            }
        }

        auto snap = analytics_.risk_view();
        auto action = risk_manager_.check_order(*o, portfolio_, snap);
        // Backtest research only: portfolio risk breaches reject the trade —
        // never stop the market replay. Live/shadow keep terminal halt even if
        // the soft flag was left true by misconfiguration.
        const bool soft_pf = config_.risk_soft_portfolio_limits
            && config_.mode == engine_mode::backtest;
        if (action == risk_action::halt && soft_pf)
            action = risk_action::reject;
        if (action == risk_action::halt || action == risk_action::reject)
        {
            const char* reason = (action == risk_action::halt)
                ? "risk limit breached - engine halted"
                : (soft_pf
                       ? "order rejected by risk manager (soft portfolio limits)"
                       : "order rejected by risk manager");

            auto rej = acquire_pooled(rejection_pool_,
                o->get_timestamp(), o->get_symbol(), o->get_order_id(), reason);
            log_event(*rej);
            publish_event(rej);

            // Migrated to sink (PR-04)
            audit_sink_->record_order_submitted(*o, "rejected");
            audit_sink_->record_rejection(*o,
                (action == risk_action::halt) ? "risk_halt" : "risk_reject",
                reason);
            audit_sink_->record_event(
                "risk_decision",
                o->get_symbol().c_str(),
                o->get_strategy_name().c_str(),
                o->get_order_id(),
                (action == risk_action::halt) ? "halt" : "reject",
                reason,
                "{}"
            );

            order_tracker_.set_status(o->get_order_id(), order_status::rejected);
            if (dashboard_builder_) dashboard_builder_->erase_open_order(o->get_order_id());
            if (action == risk_action::halt)
            {
                // Terminal process-wide halt: set halt_flag_ so DataBridge,
                // L2 dispatch, and run loops all stop — not just the local
                // halt_requested out-param (S3: halt is write-once terminal).
                if (config_.risk_unwind)
                    unwind_positions(event_count);
                trigger_halt(reason);
                halt_requested = true;
                return false;
            }
            return true;
        }
    }

    auto adapter = router_->resolve_adapter(o->get_symbol());
    const bool async_submit = router_->is_async_submit(adapter.get());

    order_tracker_.set_status(o->get_order_id(),
        async_submit ? order_status::pending : order_status::open);
    if (dashboard_builder_) {
        dashboard_builder_->cache_open_order(*o);
        if (async_submit)
            dashboard_builder_->update_open_order_status(o->get_order_id(), "submit_pending");
    }
    log_event(*o);
    publish_event(o);
    analytics_.on_event(o);

    // Migrated to sink (PR-04)
    audit_sink_->record_order_submitted(*o, "pending");
    if (!async_submit)
    {
        audit_sink_->record_status_transition(o->get_order_id(),
            order_status::pending, order_status::open);
    }
    audit_sink_->record_event(
        "order_intent",
        o->get_symbol().c_str(),
        o->get_strategy_name().c_str(),
        o->get_order_id(),
        "info",
        "order generated by strategy",
        "{}"
    );

    adapter->set_mid_price(last_mid_price_.load(std::memory_order_relaxed));
    adapter->set_l2_seeded(l2_seeded_symbols_.count(o->get_symbol()) > 0);

    router_->submit(*o, adapter.get());

    drain_async_submit_results(adapter.get());

    if (config_.mode == engine_mode::shadow && config_.provider)
    {
        auto exchange_adapter = config_.provider->get_execution_adapter();
        if (exchange_adapter)
        {
            exchange_adapter->submit_order(*o);
            drain_async_submit_results(exchange_adapter.get());
        }
    }

    if (!process_adapter_fills(adapter, event_count, halt_requested))
        return false;

    if (config_.mode == engine_mode::shadow && config_.provider)
    {
        auto exchange_adapter = config_.provider->get_execution_adapter();
        if (exchange_adapter)
        {
            drain_async_submit_results(exchange_adapter.get());
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
    if (!adapter)
        return true;

    std::vector<fill_event> fills;
    if (!router_->poll_fills(adapter.get(), fills))
        return true;

    DEBUG_STAGE(stage_timer_, fill_processing);
    const bool mark_sim = (config_.mode == engine_mode::shadow);
    for (auto& f : fills)
    {
        if (!handle_engine_fill(f, event_count, halt_requested,
                                /*run_post_fill_risk=*/true,
                                /*mark_shadow_sim=*/mark_sim))
            return false;
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
    // Virtual dispatch: LocalBookAdapter records fills; HybridExecutor
    // forwards to its inner book adapter; live bridges no-op.
    auto it = execution_adapters_.find(symbol);
    if (it == execution_adapters_.end() || !it->second)
        return;
    it->second->on_book_trades(trs, ts);
    process_adapter_fills(it->second, event_count, halt_requested);
}

bool engine::cancel_order(const std::string& symbol, uint64_t order_id,
                          const std::string& reason)
{
    auto adapter = router_->resolve_adapter(symbol);

    drain_async_submit_results(adapter.get());

    bool cancelled = adapter->cancel_order(order_id);

    if (cancelled && adapter->supports_async_submit())
    {
        pending_cancels_[order_id] = pending_cancel_meta{symbol, reason};
        if (dashboard_builder_) dashboard_builder_->update_open_order_status(order_id, "cancel_pending");
        return true;
    }

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
        if (dashboard_builder_) dashboard_builder_->erase_open_order(order_id);
        // Prefer last sim time (EL-CANCEL-WALLCLOCK); wall clock only if no event yet.
        const auto cancel_ts = (last_sim_time_.time_since_epoch().count() != 0)
            ? last_sim_time_
            : std::chrono::system_clock::now();
        auto cancel_ev = acquire_pooled(cancel_pool_,
            cancel_ts, symbol, order_id, reason);
        log_event(*cancel_ev);
        publish_event(cancel_ev);
        if (!config_.is_threaded())
            analytics_.on_event(cancel_ev);
        // Unconditional via audit_sink (replaces questdb guard + #ifdef).
        audit_sink_->record_cancellation(order_id, symbol.c_str(),
            lookup_strategy_name(order_id).c_str(),
            reason.empty() ? "manual" : reason.c_str());
        audit_sink_->record_status_transition(order_id,
            order_status::open, order_status::cancelled, reason.empty() ? nullptr : reason.c_str());
    }

    return cancelled;
}

bool engine::modify_order(const std::string& symbol, uint64_t order_id,
                          double new_price, double new_qty)
{
    // S3: no amend of resting live orders after process-wide terminal halt
    // or operator pause (new risk / size changes must not sneak through).
    if (halt_flag_.load(std::memory_order_acquire) ||
        pause_all_.load(std::memory_order_acquire))
        return false;

    auto adapter = router_->resolve_adapter(symbol);
    bool modified = adapter->modify_order(order_id, new_price, new_qty);

    if (modified)
    {
        const auto now = (last_sim_time_.time_since_epoch().count() != 0)
            ? last_sim_time_
            : std::chrono::system_clock::now();
        auto amend_ev = acquire_pooled(amend_pool_,
            now, symbol, order_id, new_price, new_qty);
        log_event(*amend_ev);
        publish_event(amend_ev);
        if (!config_.is_threaded())
            analytics_.on_event(amend_ev);
        // Unconditional via audit_sink (replaces questdb guard + #ifdef).
        // Engine doesn't preserve old price/qty cleanly here; log zeros
        // and rely on the orders/order_status tables for history.
        audit_sink_->record_amendment(order_id, symbol.c_str(),
            /*old_price=*/0.0, new_price,
            /*old_qty=*/0.0, new_qty, now);
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
            close_qty, last_mid_price_.load(std::memory_order_relaxed)));
        close_order->set_order_id(OrderIdGenerator::next());
        close_order->set_strategy_name("risk_unwind");

        order_tracker_.set_status(close_order->get_order_id(), order_status::open);
        if (dashboard_builder_) dashboard_builder_->cache_open_order(*close_order);
        log_event(*close_order);
        publish_event(close_order);
        analytics_.on_event(close_order);

        // Unconditional via audit_sink (replaces questdb guard + #ifdef).
        audit_sink_->record_order_submitted(*close_order, "pending");
        audit_sink_->record_status_transition(close_order->get_order_id(),
            order_status::pending, order_status::open, "risk_unwind");

        auto adapter = router_->resolve_adapter(symbol);
        adapter->set_mid_price(last_mid_price_.load(std::memory_order_relaxed));
        adapter->set_l2_seeded(l2_seeded_symbols_.count(symbol) > 0);

        router_->submit(*close_order, adapter.get());

        drain_async_submit_results(adapter.get());

        std::vector<fill_event> fills;
        if (router_->poll_fills(adapter.get(), fills))
        {
            bool unwind_halt = false;
            for (auto& f : fills)
            {
                // Already in halt/unwind — skip post-fill re-halt.
                (void)handle_engine_fill(f, event_count, unwind_halt,
                                         /*run_post_fill_risk=*/false,
                                         /*mark_shadow_sim=*/false,
                                         "risk_unwind");
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
    if (router_) router_->on_l2_snapshot(symbol, abid_vec, aask_vec);

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
    if (router_) router_->on_l2_update(symbol, os, price, static_cast<double>(new_qty));

    const auto l2_ts = std::chrono::system_clock::now();
    const int64_t l2_recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto ev = acquire_pooled(l2_update_pool_,
        l2_ts, symbol, ts_side, price, new_qty);
    ev->set_recv_ns(l2_recv_ns);
    log_event(*ev);
    publish_event(ev);

    /* LIVE_SAFETY_CCB_APPROVED: L2 strategy dispatch after apply + publish
       (same thread as on_tick/on_market). Halt-gated per strategy; exit
       evaluation + exit intents mirror the tick path. */
    if (pause_all_.load(std::memory_order_acquire) ||
        halt_flag_.load(std::memory_order_acquire))
        return;

    size_t l2_event_count = 0;
    bool l2_halt = false;

    // Drain pending orders eligible at this L2 timestamp. Default
    // execution_bar_delay parks strategy emissions one ns ahead of
    // sim_time; without this drain, pure L2 streams never submit.
    if (router_) router_->advance_all(l2_ts);
    drain_pending_orders(l2_ts, l2_event_count, l2_halt);
    if (l2_halt || halt_flag_.load(std::memory_order_acquire))
        return;

    // Mid/last price for ExitManager on pure L2 streams (no tick/bar).
    const double exit_px = (last_mid_price_.load(std::memory_order_relaxed) > 0.0)
        ? last_mid_price_.load(std::memory_order_relaxed)
        : price;
    if (evaluate_exits(symbol, exit_px, l2_ts, l2_event_count, l2_recv_ns))
        return;

    if (strategy_) {
        if (auto o = strategy_->on_l2_update(*ev)) {
            o->set_recv_ns(l2_recv_ns);
            o->set_strategy_name(primary_strategy_name_);
            bool route_halt = false;
            route_order(*o, l2_ts, l2_event_count, route_halt);
            finalize_strategy_route(*strategy_, primary_strategy_name_, *o, route_halt);
            if (route_halt || halt_flag_.load(std::memory_order_acquire))
                return;
        }
    }
    for (std::size_t i = 0; i < additional_strategies_.size(); ++i) {
        if (halt_flag_.load(std::memory_order_acquire))
            return;
        auto& s = additional_strategies_[i];
        if (!s) continue;
        if (auto o = s->on_l2_update(*ev)) {
            o->set_recv_ns(l2_recv_ns);
            o->set_strategy_name(additional_strategy_names_[i]);
            bool route_halt = false;
            route_order(*o, l2_ts, l2_event_count, route_halt);
            finalize_strategy_route(*s, additional_strategy_names_[i], *o, route_halt);
            if (route_halt || halt_flag_.load(std::memory_order_acquire))
                return;
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
    return instrument_spec_cache_ ? instrument_spec_cache_->resolve_instrument_spec(symbol) : nullptr;
}

bool engine::apply_instrument_spec(order_event& o, const instrument_spec& spec) const
{
    // Delegate (no duplication); cache owns impl + cache map.
    return instrument_spec_cache_ ? instrument_spec_cache_->apply_instrument_spec(o, spec) : true;
}

bool engine::route_order(order_event& order,
                         const std::chrono::system_clock::time_point& sim_time,
                         std::size_t& event_count, bool& halt_requested,
                         bool anchor_immediate)
{
    // Terminal halt gate: refuse new submits even if a call site forgot to
    // re-check halt_flag_ (e.g. L2 multi-strategy loop after primary halt).
    if (halt_flag_.load(std::memory_order_acquire))
    {
        halt_requested = true;
        return false;
    }

    // Operator-pause gate: intercept here so every strategy call site is
    // covered by one branch. Strategies still run (so analytics + lots
    // stay live for fills already in flight), but no new orders reach
    // the venue. The intent's order_id stays 0; finalize_strategy_route
    // drains exit intents and resyncs optimistic position gates.
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
            // Unconditional via audit_sink (replaces questdb guard + #ifdef + dead total_rejections_).
            audit_sink_->record_order_submitted(order, "rejected");
            audit_sink_->record_rejection(order, "venue_filter", reason);
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
    while (it != pending_stops_.end() &&
           !halt_requested &&
           !halt_flag_.load(std::memory_order_acquire))
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
                market_order->set_strategy_name(stop->get_strategy_name());
                market_order->set_opener_order_id(stop->get_opener_order_id());
                market_order->set_recv_ns(stop->get_recv_ns());
                market_order->set_earliest_eligible_ts(sim_time);
                if (market_order->get_tif() == time_in_force::day)
                    day_order_ids_.push_back({market_order->get_symbol(), market_order->get_order_id()});
                if (!process_order(market_order, event_count, halt_requested))
                {
                    last_mid_price_ = bar_mid;
                    return;
                }
            }
            else
            {
                auto limit_order = acquire_pooled(order_pool_,
                    sim_time, stop->get_symbol(), order_type::limit,
                    stop->get_side(), stop->get_quantity(), stop->get_price(),
                    stop->get_tif());
                limit_order->set_order_id(stop->get_order_id());
                limit_order->set_strategy_name(stop->get_strategy_name());
                limit_order->set_opener_order_id(stop->get_opener_order_id());
                limit_order->set_recv_ns(stop->get_recv_ns());
                limit_order->set_earliest_eligible_ts(sim_time);
                if (limit_order->get_tif() == time_in_force::day)
                    day_order_ids_.push_back({limit_order->get_symbol(), limit_order->get_order_id()});
                if (!process_order(limit_order, event_count, halt_requested))
                {
                    last_mid_price_ = bar_mid;
                    return;
                }
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

bool engine::handle_engine_fill(fill_event& f,
                                std::size_t& event_count,
                                bool& halt_requested,
                                bool run_post_fill_risk,
                                bool mark_shadow_sim,
                                const char* status_reason)
{
    stamp_fill_attribution(f);

    const uint64_t opener = f.get_opener_order_id();
    const std::string& strat = f.get_strategy_name();

    const auto new_status = f.is_partial()
        ? order_status::partially_filled : order_status::filled;
    order_tracker_.set_status(f.get_order_id(), new_status);
    if (dashboard_builder_) {
        dashboard_builder_->cache_fill(f);
        if (f.is_partial())
            dashboard_builder_->update_open_order_status(f.get_order_id(), "partial");
        else
            dashboard_builder_->erase_open_order(f.get_order_id());
    }
    auto fill_ptr = acquire_pooled(fill_pool_, f);
    log_event(f);
    portfolio_.on_fill(f, opener, strat);
    dispatch_fill_to_strategy(f);
    adverse_selection_.on_fill(f);
    exit_manager_.on_fill(f, opener);
    risk_manager_.on_fill(f);
    const char* src =
        (f.get_source() == fill_source::exchange)  ? "exchange"
      : (f.get_source() == fill_source::simulated) ? "simulated"
      :                                              "local";
    audit_sink_->record_fill(f, opener, strat.c_str(), src);
    audit_sink_->record_status_transition(f.get_order_id(),
        order_status::open, new_status, status_reason);
    notify_position_change_all(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
    publish_event(fill_ptr);
    analytics_.on_event(fill_ptr);

    if (mark_shadow_sim && config_.mode == engine_mode::shadow && shadow_tracker_)
        shadow_tracker_->on_simulated_fill(f);

    event_count++;

    if (run_post_fill_risk)
    {
        auto post_snap = analytics_.risk_view();
        auto post_action = risk_manager_.check_post_fill(f, portfolio_, post_snap);
        if (post_action == risk_action::halt)
        {
            // Soft backtest only: fill already applied; count and keep replaying.
            // Hard-gate on engine_mode so live/shadow never fail-open on the flag.
            const bool soft_pf = config_.risk_soft_portfolio_limits
                && config_.mode == engine_mode::backtest;
            if (soft_pf)
            {
                ++soft_post_fill_breaches_;
                analytics_.note_soft_post_fill_breach();
                // Per-event audit trail: this branch only runs on an actual
                // post-fill risk breach (not every fill), and record_event is
                // a no-op unless QuestDB persistence is active (see
                // QuestdbOrderAuditSink::record_event) — the allocation only
                // happens when a compliance reviewer would want the record.
                audit_sink_->record_event(
                    "risk_decision",
                    f.get_symbol().c_str(),
                    "",
                    f.get_order_id(),
                    "soft_post_fill",
                    "post-fill portfolio limit breached — continue (soft)",
                    "{}");
                return true;
            }
            if (config_.risk_unwind)
                unwind_positions(event_count);
            trigger_halt("risk post-fill limit breached - engine halted");
            halt_requested = true;
            return false;
        }
    }
    return true;
}

void engine::finalize_strategy_route(IStrategy& strategy,
                                     const std::string& strategy_name,
                                     const order_event& order,
                                     bool halted)
{
    if (halted)
    {
        // Drop pending exit intents; do not arm brackets after terminal halt.
        (void)strategy.take_pending_exit_intents();
        return;
    }

    const uint64_t oid = order.get_order_id();
    if (oid == 0)
    {
        // Paused / never assigned — drain intents and unlock optimistic gates.
        (void)strategy.take_pending_exit_intents();
        notify_position_change_all(order.get_symbol(),
                                   portfolio_.position_open(order.get_symbol()));
        return;
    }

    const auto st = order_tracker_.get_order_status(oid);
    if (st == order_status::rejected)
    {
        // Venue/risk reject after id assignment — do not arm exits; resync gates.
        (void)strategy.take_pending_exit_intents();
        notify_position_change_all(order.get_symbol(),
                                   portfolio_.position_open(order.get_symbol()));
        return;
    }

    register_strategy_exit_intent(strategy, strategy_name, order);
}

void engine::dispatch_fill_to_strategy(const fill_event& f)
{
    const std::string& name = lookup_strategy_name(f.get_order_id());
    const std::uint64_t opener = lookup_opener(f.get_order_id());

    // Empty strategy_name is common when callers omit set_primary_strategy_name
    // (MonteCarlo, C API, many tests). Still deliver to primary so strategy
    // on_fill runs — required for FR-08 partial-fill qty reconcile.
    if (strategy_ && (name.empty() || name == primary_strategy_name_))
    {
        strategy_->on_fill(f, opener);
        return;
    }
    if (name.empty()) return;
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
    auto* cap = adapter ? adapter->get_async_support() : nullptr;
    if (!cap) return;

    std::vector<synth_meta> meta;
    if (!cap->poll_synth_meta(meta)) return;

    for (const auto& m : meta)
    {
        order_meta_[m.engine_order_id] = order_meta{
            m.opener_order_id, m.strategy_name};
    }
}

void engine::drain_async_submit_results(IExecutionAdapter* adapter)
{
    auto* cap = adapter ? adapter->get_async_support() : nullptr;
    if (!cap) return;

    std::vector<submit_result> results;
    if (!cap->poll_submit_results(results)) return;

    for (const auto& sr : results)
    {
        if (sr.op == submit_result::operation::submit)
        {
            if (sr.ok)
            {
                if (order_tracker_.get_order_status(sr.engine_id) == order_status::pending)
                {
                    order_tracker_.set_status(sr.engine_id, order_status::open);
                    if (dashboard_builder_) dashboard_builder_->update_open_order_status(sr.engine_id, "open");
                    // Unconditional via audit_sink (replaces questdb guard + #ifdef).
                    audit_sink_->record_status_transition(sr.engine_id,
                        order_status::pending, order_status::open,
                        "venue submit acknowledged");
                }
                continue;
            }
            if (!order_tracker_.is_active(sr.engine_id)) continue;

            auto rej = acquire_pooled(rejection_pool_,
                std::chrono::system_clock::now(), sr.symbol, sr.engine_id,
                "submit failed: " + sr.error);
            log_event(*rej);
            publish_event(rej);
            order_tracker_.set_status(sr.engine_id, order_status::rejected);
            if (dashboard_builder_) dashboard_builder_->erase_open_order(sr.engine_id);
            // Unconditional via audit_sink using the single record_rejection shape
            // (the rich order_event overload). For async submit transport errors
            // we synthesize a minimal stack order_event carrying the identity we have
            // (id + symbol + looked-up strategy). qty/price/side are best-effort zeros
            // (the sink path will record zeros for qty/price as before).
            // Strategy lookup mirrors the pattern used for cancellations in the same drain.
            const std::string& strat = lookup_strategy_name(sr.engine_id);
            order_event ghost{
                std::chrono::system_clock::now(),
                sr.symbol,
                order_type::market,
                order_side::buy,
                0.0,
                0.0
            };
            ghost.set_order_id(sr.engine_id);
            if (!strat.empty())
                ghost.set_strategy_name(strat);

            char transport_msg[128];
            std::snprintf(transport_msg, sizeof(transport_msg), "transport_error: %s", sr.error.c_str());
            audit_sink_->record_status_transition(sr.engine_id,
                order_status::pending, order_status::rejected,
                transport_msg);
            audit_sink_->record_rejection(ghost, "transport_error", sr.error.c_str());
            continue;
        }

        auto meta_it = pending_cancels_.find(sr.engine_id);
        const std::string symbol =
            !sr.symbol.empty() ? sr.symbol :
            (meta_it != pending_cancels_.end() ? meta_it->second.symbol : "");
        const std::string reason =
            (meta_it != pending_cancels_.end() && !meta_it->second.reason.empty())
                ? meta_it->second.reason
                : (sr.ok ? "venue cancel acknowledged" : "venue cancel failed");

        if (sr.ok)
        {
            if (order_tracker_.is_active(sr.engine_id))
            {
                order_tracker_.set_status(sr.engine_id, order_status::cancelled);
                if (dashboard_builder_) dashboard_builder_->erase_open_order(sr.engine_id);
                auto cancel_ev = acquire_pooled(cancel_pool_,
                    std::chrono::system_clock::now(), symbol, sr.engine_id, reason);
                log_event(*cancel_ev);
                publish_event(cancel_ev);
                if (!config_.is_threaded())
                    analytics_.on_event(cancel_ev);
                // Unconditional via audit_sink (replaces questdb guard + #ifdef).
                audit_sink_->record_cancellation(sr.engine_id, symbol.c_str(),
                    lookup_strategy_name(sr.engine_id).c_str(),
                    reason.empty() ? "manual" : reason.c_str());
                audit_sink_->record_status_transition(sr.engine_id,
                    order_status::open, order_status::cancelled, reason.empty() ? nullptr : reason.c_str());
            }
        }
        else
        {
            if (dashboard_builder_) dashboard_builder_->update_open_order_status(sr.engine_id, "cancel_failed");
        }

        if (meta_it != pending_cancels_.end())
            pending_cancels_.erase(meta_it);
    }
}

void engine::register_strategy_exit_intent(IStrategy& strategy,
                                           const std::string& strategy_name,
                                           const order_event& order)
{
    const std::uint64_t order_id = order.get_order_id();
    if (order_id == 0)
    {
        // Opener not assigned (pause/drop) — drain so intents cannot leak
        // and re-arm on a later unrelated entry.
        (void)strategy.take_pending_exit_intents();
        return;
    }
    auto intents = strategy.take_pending_exit_intents();

    // Platform floor / engine_only / union: attach protective SL/TP for any
    // strategy that omitted them. Position-reducing signal closes are skipped
    // so death-cross sells do not arm inverted short brackets.
    double net_qty = 0.0;
    {
        const auto& positions = portfolio_.get_positions();
        auto it = positions.find(order.get_symbol());
        if (it != positions.end())
            net_qty = it->second.qty;
    }
    intents = truetest::exits::apply_default_exit_policy(
        config_.exit_defaults, order, net_qty, std::move(intents));

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
                                  std::size_t& event_count, bool& halt_requested,
                                  double bar_volume)
{
    auto it = execution_adapters_.find(symbol);
    if (it == execution_adapters_.end() || !it->second)
        return;
    // Virtual dispatch (same capability surface as deliver_mm_book_trades).
    if (it->second->sweep_resting_range(symbol, low, high, ts, bar_volume))
        process_adapter_fills(it->second, event_count, halt_requested);
}

void engine::dispatch_extras_on_market(const market_event& mkt,
                                       const std::chrono::system_clock::time_point& ts,
                                       std::size_t& event_count)
{
    if (additional_strategies_.empty()) return;
    for (std::size_t i = 0; i < additional_strategies_.size(); ++i)
    {
        if (halt_flag_.load(std::memory_order_acquire)) return;
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
            finalize_strategy_route(*s, additional_strategy_names_[i], *o, halt);
            if (halt || halt_flag_.load(std::memory_order_acquire)) return;
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
        if (halt_flag_.load(std::memory_order_acquire)) return;
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
            finalize_strategy_route(*s, additional_strategy_names_[i], *o, halt);
            if (halt || halt_flag_.load(std::memory_order_acquire)) return;
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

    // Already terminal (e.g. risk halt on a prior event / DataBridge race):
    // do not strategy-emit or submit on this bar.
    if (halt_flag_.load(std::memory_order_acquire))
        return;

    // Operator-requested flatten: drain on the next event so the timestamp
    // we close at is from the live stream rather than wall-clock-now.
    if (flatten_request_.exchange(false, std::memory_order_acq_rel))
        unwind_positions(event_count);

    // Advance adapter clocks first so cancels whose in-flight window has
    // elapsed are drained before this event's matching runs.
    if (router_) router_->advance_all(timestamp);

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

    last_sim_time_ = timestamp;
    last_mid_price_.store(mkt.get_open(), std::memory_order_release);
    last_mark_symbol_ = mkt.get_symbol();
    { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[mkt.get_symbol()] = mkt.get_open(); }

    // Drain delayed orders at open mid for each order's symbol (not the
    // event symbol alone — multi-symbol pending must not walk the wrong book).
    bool halt = false;
    drain_pending_orders(timestamp, event_count, halt);
    // Risk halt (or other terminal) during pending drain: do not continue into
    // strategy / route_order on this bar (was previously loop-scoped only).
    if (halt || halt_flag_.load(std::memory_order_acquire))
        return;

    last_mid_price_.store(mkt.get_close(), std::memory_order_release);
    last_mark_symbol_ = mkt.get_symbol();
    { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[mkt.get_symbol()] = mkt.get_close(); }

    {
        // Single stop pass (EL-STREAM-DOUBLE-STOPS): matches batch run().
        // Stops + bar-range sweep before MM/provider fills.
        bool halt = false;
        check_pending_stops(mkt.get_open(), mkt.get_high(), mkt.get_low(),
                            timestamp, event_count, halt);
        sweep_resting_limits(mkt.get_symbol(), mkt.get_low(), mkt.get_high(),
                             timestamp, event_count, halt,
                             static_cast<double>(mkt.get_volume()));
        // Match tick/history paths: do not generate new MM/provider fills
        // after a terminal halt on this bar.
        if (halt || halt_flag_.load(std::memory_order_acquire))
            return;
    }

    auto ob = orderbook_registry_.get_or_create(mkt.get_symbol());
    if (!mm_worker_ &&
        !l2_seeded_symbols_.count(mkt.get_symbol()))
    {
        auto mm_trades = market_maker_.replenish(
            ob, last_mid_price_.load(std::memory_order_relaxed));
        bool halt = false;
        deliver_mm_book_trades(mkt.get_symbol(), mm_trades,
                               timestamp, event_count, halt);
    }

    // Paper maker-queue: synthetic trade at bar close (lossy, matches shadow bar).
    {
        bool halt_paper = false;
        feed_paper_trade_and_drain(mkt.get_symbol(), mkt.get_close(),
                                   static_cast<double>(mkt.get_volume()),
                                   timestamp, event_count, halt_paper);
        if (halt_paper || halt_flag_.load(std::memory_order_acquire))
            return;
    }

    if (config_.provider && config_.provider->has_execution())
    {
        config_.provider->on_mid_price(mkt.get_symbol(), last_mid_price_.load(std::memory_order_relaxed));
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
        drain_async_submit_results(provider_adapter.get());
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
                // Live/async fills: full canonical pipeline (risk + tracker + audit).
                bool fill_halt = false;
                if (!handle_engine_fill(f, event_count, fill_halt))
                    return;
            }
        }
    }

    // Stops already evaluated once above (EL-STREAM-DOUBLE-STOPS).
    // Canonical order continues: exits → strategy → route.

    auto mkt_ptr = acquire_pooled(market_pool_,mkt);
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

    if (!strategy_ || halt_flag_.load(std::memory_order_acquire))
        return;

    auto order_opt = strategy_->on_market(mkt);
    if (order_opt && !halt_flag_.load(std::memory_order_acquire))
    {
        order_opt->set_recv_ns(mkt.get_recv_ns());
        if (!primary_strategy_name_.empty())
            order_opt->set_strategy_name(primary_strategy_name_);
        bool route_halt = false;
        route_order(*order_opt, timestamp, event_count, route_halt);
        finalize_strategy_route(*strategy_, primary_strategy_name_, *order_opt, route_halt);
    }
    if (!halt_flag_.load(std::memory_order_acquire))
        dispatch_extras_on_market(mkt, timestamp, event_count);
}

void engine::process_single_tick(const tick_record& rec, std::size_t& event_count)
{
    drain_object_pool_returns();

    if (halt_flag_.load(std::memory_order_acquire))
        return;

    if (flatten_request_.exchange(false, std::memory_order_acq_rel))
        unwind_positions(event_count);

    if (router_) router_->advance_all(rec.timestamp);

    tick_side ts = tick_side::unknown;
    if (rec.side == data_tick_side::bid) ts = tick_side::bid;
    else if (rec.side == data_tick_side::ask) ts = tick_side::ask;

    tick_event te(rec.timestamp, rec.symbol, rec.price, rec.quantity, ts);
    te.set_recv_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    last_sim_time_ = rec.timestamp;
    last_mid_price_.store(rec.price, std::memory_order_release);
    last_mark_symbol_ = rec.symbol;
    { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[rec.symbol] = rec.price; }

    {
        DEBUG_STAGE(stage_timer_, mm_replenish);
        auto ob = orderbook_registry_.get_or_create(rec.symbol);
        if (!l2_seeded_symbols_.count(rec.symbol))
        {
            auto mm_trades = market_maker_.replenish(
                ob, last_mid_price_.load(std::memory_order_relaxed));
            bool halt = false;
            deliver_mm_book_trades(rec.symbol, mm_trades,
                                   rec.timestamp, event_count, halt);
        }
    }

    // Paper maker-queue: real tick print advances QueueAware size_ahead.
    {
        bool halt_paper = false;
        feed_paper_trade_and_drain(rec.symbol, rec.price,
                                   static_cast<double>(rec.quantity),
                                   rec.timestamp, event_count, halt_paper);
        if (halt_paper || halt_flag_.load(std::memory_order_acquire))
            return;
    }

    if (config_.provider && config_.provider->has_execution())
    {
        config_.provider->on_mid_price(rec.symbol, last_mid_price_.load(std::memory_order_relaxed));
        auto provider_adapter = config_.provider->get_execution_adapter();

        // Must fire BEFORE poll_fills so fills this tick are drained here.
        if (config_.mode == engine_mode::shadow && provider_adapter)
        {
            provider_adapter->on_trade(rec.symbol, rec.price,
                                       static_cast<double>(rec.quantity),
                                       rec.timestamp);
        }

        drain_venue_bracket_meta();
        drain_async_submit_results(provider_adapter.get());
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
                // Live/async fills: full canonical pipeline (risk + tracker + audit).
                bool fill_halt = false;
                if (!handle_engine_fill(f, event_count, fill_halt))
                    return;
            }
        }
    }

    bool halt = false;

    {
        DEBUG_STAGE(stage_timer_, pending_drain);
        drain_pending_orders(rec.timestamp, event_count, halt);
    }
    if (halt || halt_flag_.load(std::memory_order_acquire)) return;

    {
        DEBUG_STAGE(stage_timer_, stop_check);
        check_pending_stops(rec.price, rec.price, rec.price, rec.timestamp, event_count, halt);
    }
    if (halt || halt_flag_.load(std::memory_order_acquire)) return;

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

    if (!strategy_ || halt_flag_.load(std::memory_order_acquire))
        return;

    std::optional<order_event> order_opt;
    {
        DEBUG_STAGE(stage_timer_, strategy);
        order_opt = strategy_->on_tick(te);
    }
    if (order_opt && !halt_flag_.load(std::memory_order_acquire))
    {
        order_opt->set_recv_ns(te.get_recv_ns());
        if (!primary_strategy_name_.empty())
            order_opt->set_strategy_name(primary_strategy_name_);
        route_order(*order_opt, rec.timestamp, event_count, halt);
        finalize_strategy_route(*strategy_, primary_strategy_name_, *order_opt, halt);
    }
    if (!halt_flag_.load(std::memory_order_acquire))
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
    prepare_mark_prices_for_run(/*symbol_hint=*/16);
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
    std::chrono::system_clock::time_point last_good_bar_ts{};

    bridge->run_streaming(data_handler_, [&](const bar_record& rec) {
        auto timestamp = tt::date_parse::resolve_bar_clock(
            rec.open_time_ms, rec.date, last_good_bar_ts);
        last_good_bar_ts = timestamp;
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
                maybe_questdb_tick();
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

    // Match batch EOS: drain delayed pending + cancel DAY residuals (EL-03).
    {
        bool halt = halt_flag_.load(std::memory_order_acquire);
        drain_final_pending(event_count, halt);
        cancel_day_orders();
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
    prepare_mark_prices_for_run(/*symbol_hint=*/16);
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
                maybe_questdb_tick();
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

    {
        bool halt = halt_flag_.load(std::memory_order_acquire);
        drain_final_pending(event_count, halt);
        cancel_day_orders();
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
    prepare_mark_prices_for_run(/*symbol_hint=*/16);
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
                auto timestamp = tt::date_parse::resolve_bar_clock(
                    rec.open_time_ms, rec.date, current_event_ts);
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
            if (last_mid_price_.load(std::memory_order_relaxed) > 0.0)
                st.last_price_fp8.store(
                    static_cast<std::int64_t>(last_mid_price_.load(std::memory_order_relaxed) * 1e8),
                    std::memory_order_relaxed);
            st.realized_pnl_fp4.store(
                static_cast<std::int64_t>(std::llround(analytics_.realized_pnl() * 1e4)),
                std::memory_order_relaxed);
            // Only mark once we've seen a price — L2/status may arrive first.
            if (last_mid_price_.load(std::memory_order_relaxed) > 0.0 && !last_mark_symbol_.empty())
            {
                const auto& positions = portfolio_.get_positions();
                double qty = 0.0, cost = 0.0;
                if (auto it = positions.find(last_mark_symbol_); it != positions.end())
                {
                    qty  = it->second.qty;
                    cost = it->second.cost_basis;
                }
                const double unreal = qty * last_mid_price_.load(std::memory_order_relaxed) - cost;
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
            if (last_mid_price_.load(std::memory_order_relaxed) > 0.0 && !last_mark_symbol_.empty())
            {
                adverse_selection_.on_mark(last_mark_symbol_,
                                           last_mid_price_.load(std::memory_order_relaxed),
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
                maybe_questdb_tick();
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

    {
        bool halt = halt_flag_.load(std::memory_order_acquire);
        drain_final_pending(event_count, halt);
        cancel_day_orders();
    }

    if (event_logger_) event_logger_->flush();
    stop_workers();
#ifdef HAS_QUESTDB
    questdb_end();
#endif
}

void engine::run()
{
    // One of four similar run* methods. Duplicated event-loop skeleton (pending clear,
    // workers, pin, questdb, progress, drains, teardown) targeted for Wave 2 refactor
    // (shared run_event_loop or thin EventLoopCoordinator) per core/docs/internal/engine-decomposition.md#E-40
    // + engine-decomposition skill. Cold extraction only; hot paths unchanged.
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    if (data_handler_->has_tick_data())
    {
        run_tick_data();
        return;
    }

    if (!data_handler_->has_bar_data()) {
        throw std::runtime_error("no data loaded — call IMarketSource::load_into() / DataWrapper::load() before run()");
    }

    if (!config_.event_log_path.empty())
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path, config_.compress_log);

#ifdef HAS_DEBUG
    memory_sampler_.set_start(debug::memory_snapshot::capture());
#endif

    setup_event_loop_infra();

    const auto base_ts = (config_.seed != 0)
        ? std::chrono::system_clock::time_point(std::chrono::milliseconds(0))
        : std::chrono::system_clock::now();
    // docs/internal/data-pipeline.md#D-02: engine batch loop uses MarketSeries read API only.
    const auto n = data_handler_->bar_count();
    analytics_.reserve_hint(n);
    prepare_mark_prices_for_run(/*symbol_hint=*/16);
    const auto start = std::chrono::high_resolution_clock::now();
    if (config_.show_progress) {
        std::cout << "\rProgress: 0.000% | Trades executed: 0" << std::flush;
    }

    std::size_t event_count = 0;
    auto last_report_time = std::chrono::steady_clock::now();

    clear_pending_state();

    bool halt_requested = false;

    // Prefer stored bar timestamps (open_time / date parsed at load) whenever
    // present and monotonic. Seed no longer forces synthetic +1ms bar clock —
    // it remains for fill/MM RNG only. Fall back to base+i / +1ms when ts is
    // missing or non-monotonic (legacy golden / synthetic series without ts).
    // docs/internal/data-pipeline.md#D-10 — LIVE_SAFETY_CCB_APPROVED: narrow resolve_bar_ts only.
    auto resolve_bar_ts = [&](std::size_t i,
                              const std::chrono::system_clock::time_point& prev,
                              const MarketSeries::BarView& bar)
        -> std::chrono::system_clock::time_point
    {
        if (bar.ts != std::chrono::system_clock::time_point{})
        {
            if (i == 0 || bar.ts > prev) return bar.ts;
        }
        // Fallback: parse date view only if ts was not stored
        if (auto parsed = tt::date_parse::parse(bar.date))
        {
            if (i == 0 || *parsed > prev) return *parsed;
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
        const auto bar = data_handler_->bar_at(i);
        auto this_bar_ts = resolve_bar_ts(i, prev_bar_ts, bar);
        prev_bar_ts = this_bar_ts;
        // bar_symbol_at: const string& into series — no temporary on hot path
        market_event mkt(
            this_bar_ts,
            data_handler_->bar_symbol_at(i),
            bar.open,
            bar.high,
            bar.low,
            bar.close,
            bar.volume
        );
        mkt.set_recv_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        auto sim_time = mkt.get_timestamp();
        const auto& symbol = mkt.get_symbol();
        last_sim_time_ = sim_time;

        last_mid_price_.store(mkt.get_open(), std::memory_order_release);
        { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[symbol] = mkt.get_open(); }

        // Advance adapter clocks so latency-gated cancels complete offline.
        if (router_) router_->advance_all(sim_time);

        {
            DEBUG_STAGE(stage_timer_, pending_drain);
            // Per-order mid + book re-center (open mark already stored above).
            drain_pending_orders(sim_time, event_count, halt_requested);
        }
        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

        last_mid_price_.store(mkt.get_close(), std::memory_order_release);
        { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[symbol] = mkt.get_close(); }

        {
            DEBUG_STAGE(stage_timer_, stop_check);
            check_pending_stops(mkt.get_open(), mkt.get_high(), mkt.get_low(), sim_time, event_count, halt_requested);
        }
        sweep_resting_limits(symbol, mkt.get_low(), mkt.get_high(),
                             sim_time, event_count, halt_requested,
                             static_cast<double>(mkt.get_volume()));

        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

        feed_paper_trade_and_drain(symbol, mkt.get_close(),
                                   static_cast<double>(mkt.get_volume()),
                                   sim_time, event_count, halt_requested);
        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

        // Drain async venue fills that arrived after a prior submit's
        // process_order poll (live/async). Same canonical handle_engine_fill
        // path as process_single_bar/tick.
        if (config_.provider && config_.provider->has_execution())
        {
            config_.provider->on_mid_price(symbol,
                last_mid_price_.load(std::memory_order_relaxed));
            auto provider_adapter = config_.provider->get_execution_adapter();
            drain_venue_bracket_meta();
            drain_async_submit_results(provider_adapter.get());
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
                            auto fill_ptr = acquire_pooled(fill_pool_, f);
                            exchange_analytics_->on_event(fill_ptr);
                        }
                        continue;
                    }
                    if (!handle_engine_fill(f, event_count, halt_requested))
                        break;
                }
            }
        }
        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

        {
            DEBUG_STAGE(stage_timer_, mm_replenish);
            auto ob = orderbook_registry_.get_or_create(symbol);
            if (!mm_worker_ &&
                !l2_seeded_symbols_.count(symbol))
            {
                auto mm_trades = market_maker_.replenish(
                    ob, last_mid_price_.load(std::memory_order_relaxed));
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

        // Canonical: exits before strategy decision (matches tick path).
        if (evaluate_exits(symbol,
                           mkt.get_open(), mkt.get_low(), mkt.get_high(), mkt.get_close(),
                           sim_time, event_count, mkt.get_recv_ns()))
            break;

        if (!strategy_ || halt_flag_.load(std::memory_order_acquire))
            break;

        std::optional<order_event> order_opt;
        {
            DEBUG_STAGE(stage_timer_, strategy);
            order_opt = strategy_->on_market(mkt);
        }

        if (order_opt)
        {
            order_opt->set_recv_ns(mkt.get_recv_ns());
            if (!primary_strategy_name_.empty())
                order_opt->set_strategy_name(primary_strategy_name_);
            route_order(*order_opt, sim_time, event_count, halt_requested);
            finalize_strategy_route(*strategy_, primary_strategy_name_, *order_opt,
                                    halt_requested);
        }
        if (!halt_flag_.load(std::memory_order_acquire))
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
                maybe_questdb_tick();
            }
        }

        write_checkpoint_if_due(event_count);
    }

    drain_final_pending(event_count, halt_requested);
    cancel_day_orders();

    report_run_summary(event_count, start);

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

    teardown_event_loop_infra();

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

    clear_pending_state();

#ifdef HAS_DEBUG
    memory_sampler_.set_start(debug::memory_snapshot::capture());
#endif

    setup_event_loop_infra();

    // docs/internal/data-pipeline.md#D-02: tick path uses tick_at / tick_count (no public vector).
    const auto n = data_handler_->tick_count();
    prepare_mark_prices_for_run(/*symbol_hint=*/16);
    const auto start = std::chrono::high_resolution_clock::now();

    if (config_.show_progress) {
        std::cout << "\rProgress: 0.000% | Trades executed: 0" << std::flush;
    }

    std::size_t event_count = 0;
    bool halt_requested = false;
    auto last_report_time = std::chrono::steady_clock::now();

    // Tick path: aggregate bars for analytics/marks only. Strategy market
    // handling is on_tick exclusively — dual on_tick + on_market double-fired
    // indicators/entries on strategies that implement both (EL-01).
    // Exit evaluation is per-tick only (EL-TICK-BAR-EXIT-LA): synthetic 1s
    // bars emit the completed prior interval after a later tick arrives, so
    // OHLC on_bar would test adverse extremes printed before entry arm and
    // look-ahead stop-out vs pure on_price.
    BarAggregator bar_agg(std::chrono::seconds(1), [&](const market_event& bar)
    {
        int64_t bar_recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        auto bar_ptr = acquire_pooled(market_pool_,bar);
        bar_ptr->set_recv_ns(bar_recv_ns);
        publish_event(bar_ptr);
        if (!config_.is_threaded())
            analytics_.on_event(bar_ptr);
        else
            analytics_.on_mark(bar.get_symbol(), bar.get_close());
        (void)halt_requested;
    });

    for (std::size_t i = 0; i < n && !halt_requested
             && !halt_flag_.load(std::memory_order_acquire)
             && !worker_failed_.load(std::memory_order_acquire); ++i)
    {
        const auto& tick = data_handler_->tick_at(i);

        last_sim_time_ = tick.timestamp;
        last_mid_price_.store(tick.price, std::memory_order_release);
        { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[tick.symbol] = tick.price; }

        // Latency-gated cancel windows need clock advance on the tick path too.
        if (router_) router_->advance_all(tick.timestamp);

        {
            DEBUG_STAGE(stage_timer_, mm_replenish);
            auto ob = orderbook_registry_.get_or_create(tick.symbol);
            if (!l2_seeded_symbols_.count(tick.symbol))
            {
                auto mm_trades = market_maker_.replenish(
                    ob, last_mid_price_.load(std::memory_order_relaxed));
                deliver_mm_book_trades(tick.symbol, mm_trades, tick.timestamp,
                                       event_count, halt_requested);
            }
        }

        {
            DEBUG_STAGE(stage_timer_, pending_drain);
            drain_pending_orders(tick.timestamp, event_count, halt_requested);
        }
        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

        {
            DEBUG_STAGE(stage_timer_, stop_check);
            check_pending_stops(tick.price, tick.price, tick.price, tick.timestamp, event_count, halt_requested);
        }
        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

        feed_paper_trade_and_drain(tick.symbol, tick.price,
                                   static_cast<double>(tick.quantity),
                                   tick.timestamp, event_count, halt_requested);
        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

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

        // EL-TICK-NULL-STRATEGY: match process_single_tick / run() null guard.
        if (!strategy_ || halt_flag_.load(std::memory_order_acquire))
        {
            bar_agg.on_tick(tick.symbol, tick.price, tick.quantity, tick.timestamp);
            continue;
        }

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
            if (strategy_)
                finalize_strategy_route(*strategy_, primary_strategy_name_, *order_opt,
                                        halt_requested);
        }
        if (!halt_flag_.load(std::memory_order_acquire))
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
                maybe_questdb_tick();
            }
        }
    }

    bar_agg.flush();

    drain_final_pending(event_count, halt_requested);
    cancel_day_orders();

    report_run_summary(event_count, start);

    teardown_event_loop_infra();

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

    setup_event_loop_infra();

    // Replay must go through route_order+process_order so instrument spec,
    // exec delay, latency, stops, and day-order behavior stay identical to
    // the original run (not a thinner inline path).
    clear_pending_state();
    prepare_mark_prices_for_run(/*symbol_hint=*/16);

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
            last_sim_time_ = sim_time;

            last_mid_price_.store(mkt.get_open(), std::memory_order_release);
            { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[mkt.get_symbol()] = mkt.get_open(); }
            if (router_) router_->advance_all(sim_time);

            drain_pending_orders(sim_time, event_count, halt_requested);
            if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

            last_mid_price_.store(mkt.get_close(), std::memory_order_release);
            { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[mkt.get_symbol()] = mkt.get_close(); }

            check_pending_stops(mkt.get_open(), mkt.get_high(), mkt.get_low(), sim_time,
                                event_count, halt_requested);
            if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;
            sweep_resting_limits(mkt.get_symbol(), mkt.get_low(), mkt.get_high(),
                                 sim_time, event_count, halt_requested,
                                 static_cast<double>(mkt.get_volume()));
            if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

            // Paper maker-queue tape (parity with process_single_bar / run).
            feed_paper_trade_and_drain(mkt.get_symbol(), mkt.get_close(),
                                       static_cast<double>(mkt.get_volume()),
                                       sim_time, event_count, halt_requested);
            if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

            auto ob = orderbook_registry_.get_or_create(mkt.get_symbol());
            if (!l2_seeded_symbols_.count(mkt.get_symbol()))
            {
                auto mm_trades = market_maker_.replenish(
                    ob, last_mid_price_.load(std::memory_order_relaxed));
                deliver_mm_book_trades(mkt.get_symbol(), mm_trades, sim_time,
                                       event_count, halt_requested);
            }

            publish_event(ev);
            if (!config_.is_threaded())
                analytics_.on_event(ev);
            else
                analytics_.on_mark(mkt.get_symbol(), mkt.get_close());

            // Canonical: exits before strategy decision (matches tick path).
            if (evaluate_exits(mkt.get_symbol(),
                               mkt.get_open(), mkt.get_low(), mkt.get_high(), mkt.get_close(),
                               sim_time, event_count, mkt.get_recv_ns()))
                break;

            if (!strategy_ || halt_flag_.load(std::memory_order_acquire))
                break;

            auto order_opt = strategy_->on_market(mkt);
            if (order_opt)
            {
                if (!primary_strategy_name_.empty())
                    order_opt->set_strategy_name(primary_strategy_name_);
                order_opt->set_recv_ns(mkt.get_recv_ns());
                route_order(*order_opt, sim_time, event_count, halt_requested);
                finalize_strategy_route(*strategy_, primary_strategy_name_, *order_opt,
                                        halt_requested);
            }
            if (!halt_flag_.load(std::memory_order_acquire))
                dispatch_extras_on_market(mkt, sim_time, event_count);
            break;
        }
        case event_type::tick: {
            auto& te = static_cast<tick_event&>(*ev);
            const auto sim_time = te.get_timestamp();
            last_sim_time_ = sim_time;

            last_mid_price_.store(te.get_price(), std::memory_order_release);
            { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[te.get_symbol()] = te.get_price(); }
            if (router_) router_->advance_all(sim_time);

            drain_pending_orders(sim_time, event_count, halt_requested);
            if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

            check_pending_stops(te.get_price(), te.get_price(), te.get_price(), sim_time,
                                event_count, halt_requested);
            if (halt_requested) break;

            // Paper maker-queue tape (parity with process_single_tick / run_tick_data).
            feed_paper_trade_and_drain(te.get_symbol(), te.get_price(),
                                       static_cast<double>(te.get_quantity()),
                                       sim_time, event_count, halt_requested);
            if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

            publish_event(ev);
            if (!config_.is_threaded())
                analytics_.on_event(ev);
            else
                analytics_.on_mark(te.get_symbol(), te.get_price());

            if (evaluate_exits(te.get_symbol(), te.get_price(), sim_time,
                               event_count, te.get_recv_ns()))
                break;

            if (!strategy_ || halt_flag_.load(std::memory_order_acquire))
                break;

            auto order_opt = strategy_->on_tick(te);
            if (order_opt)
            {
                if (!primary_strategy_name_.empty())
                    order_opt->set_strategy_name(primary_strategy_name_);
                order_opt->set_recv_ns(te.get_recv_ns());
                route_order(*order_opt, sim_time, event_count, halt_requested);
                finalize_strategy_route(*strategy_, primary_strategy_name_, *order_opt,
                                        halt_requested);
            }
            if (!halt_flag_.load(std::memory_order_acquire))
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
    drain_final_pending(event_count, halt_requested);
    cancel_day_orders();

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << std::endl;
    std::cout << "Replay complete: " << event_count << " events in "
              << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

    teardown_event_loop_infra();
}
