#include "engine.h"

#include "reproducibility/deterministic_seed.h"
#include "data/quantity_scale.h"
#include "checkpoint.h"
#include "live_safety_session.h"
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
#include <limits>
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
// Authoritative path list: scripts/check-live-safety-freeze.sh
//
// Engine decomposition Phase 1 (mechanical translation-unit split; see
// core/docs/internal/engine-decomposition.md for the pre-existing object-
// extraction plan this phase deliberately does NOT continue — no new
// subsystem classes were introduced here, only file-level relocation of
// existing `engine::` method bodies). All changes must preserve zero-alloc
// hot path, identical behavior for MC reuse / backtest / shadow / live, and
// single IOrderAuditSink + ExecutionRouter seams. No direct questdb
// recording or ad-hoc adapter bypasses allowed.
//
// This file is now the map, not the city: constructor/destructor, the
// hot-path event dispatch (log_event/publish_event), the single halt/kill
// entry points, and the canonical run() event loop. Everything else lives
// in a sibling engine_*.cpp, each with one clear responsibility:
//   engine_lifecycle.cpp     — pool prewarm/drain, checkpoint write/restore,
//                               strategy wiring, reset_for_next_trial (MC).
//   engine_market.cpp        — bar/tick/L2 application + dispatch, and the
//                               run_tick_data()/run_replay()/run_streaming()
//                               event-loop pumps (run() itself stays here).
//   engine_orders.cpp        — process_order, route_order, cancel/modify,
//                               resting-order triggering, exits.
//   engine_fills.cpp         — venue bracket meta / async submit-result /
//                               funding-update draining (order/market-pipeline
//                               territory that stays engine-owned for now).
//   engine_workers.cpp       — worker/ring start/stop, pinning, QuestDB
//                               persistence activation/tick.
//   engine_observability.cpp — dashboard snapshot delegation, print_summary,
//                               small read-only accessors.
// Phase 2 engine decomposition (domain-processor extraction, 2026-08): the
// canonical fill pipeline (former engine::handle_engine_fill and friends)
// now lives in fill_processor.{h,cpp} as FillProcessor, owned by engine via
// the fills_ member and coordinating the same existing subsystems it always
// did (Portfolio, OrderTracker, ExitManager, RiskManager, ExecutionRouter,
// IOrderAuditSink, Analytics) — see
// core/docs/internal/engine-decomposition.md "Phase 2: Domain Processors"
// for the dependency list, state-ownership table, and call-order evidence.
// See docs/internal/engine-decomposition.md for the broader (unrelated,
// pre-existing) object-extraction plan; the Phase 1 TU split did not
// implement it, but Phase 2 (this and future domain-processor steps) does.
// ENGINE_LOC_WAIVER: (historical; superseded by the 2026-08 Phase 1 TU split
// below — engine.cpp is now far under limit; kept as a documented marker.)
// ============================================================

namespace
{
engine_config refuse_legacy_resume(engine_config config)
{
    if (!config.resume_checkpoint_path.empty())
        throw std::runtime_error(
            "checkpoint resume v1 is disabled; no engine state was created");
    return config;
}
}

engine::engine(std::shared_ptr<data_handler> dh,
               std::shared_ptr<orderbook> ob,
               std::shared_ptr<IStrategy> strategy,
               engine_config config)
    : config_(refuse_legacy_resume(std::move(config))), data_handler_(std::move(dh)), strategy_(std::move(strategy)),
      portfolio_(config_.initial_balance),
      order_tracker_(
          OrderTracker::default_native_execution_capacity,
          std::isfinite(config_.qty_scale) && config_.qty_scale > 0.0
              ? 1.0 / config_.qty_scale : 0.0),
      analytics_(config_.initial_balance, config_.rolling_window, config_.risk_free_rate,
                 config_.periods_per_year, config_.max_equity_points),
      risk_manager_(config_.risk),
      market_maker_(MarketMaker(
          truetest::reproducibility::DeterministicSeedDeriver(config_.seed)
              .derive(truetest::reproducibility::SeedDomain::market_maker)))
{
    if (config_.mode == engine_mode::live
        && (!config_.provider
            || !config_.live_safety_session
            || !config_.live_safety_session->owns_provider(config_.provider)
            || !config_.live_safety_session->is_open()
            || !config_.live_safety_session->startup_safety_validated()
            || !config_.live_safety_session->permits_private_exchange_writes()))
        throw std::runtime_error(
            "live provider requires its validated open pre-owned LiveSafetySession");
    if (config_.mode == engine_mode::live && config_.live_safety_session
        && !config_.live_safety_session->configured_safety_matches(
            config_.reconciler, config_.kill_switch))
        throw std::runtime_error(
            "startup rejected: engine safety configuration differs from validated session");
    if (config_.provider)
        provider_funding_ingress_ = config_.provider->funding_ingress();
    market_maker_.set_calibration({config_.mm_levels_per_side,
                                   config_.mm_base_depth,
                                   config_.mm_base_spread_pct,
                                   config_.mm_vol_spread_mult,
                                   config_.mm_max_half_spread_pct,
                                   config_.qty_scale});
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
                        if (msg.k != parsed_exec::kind::partial_fill
                            && msg.k != parsed_exec::kind::full_fill)
                            return std::nullopt;

                        const auto attribution = exit_manager_
                            .venue_fill_attribution_for_exchange_order(
                                msg.exchange_order_id);
                        if (!attribution || attribution->opener_order_id == 0)
                            return std::nullopt;
                        const auto opener = attribution->opener_order_id;
                        auto strategy_name = attribution->strategy_name;
                        const double intended_price = attribution->intended_price;

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
                        fill_provenance provenance;
                        provenance.model = fill_execution_model::venue_reported;
                        provenance.reason = fill_execution_reason::venue_execution_report;
                        provenance.intended_price = intended_price;
                        provenance.reference_price = msg.last_fill_price;
                        provenance.reference_timestamp = ts;
                        fe.set_provenance(provenance);
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
        auto err = config_.live_safety_session->reconcile(
            portfolio_, config_.reconcile_tolerance_bps);
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
            auto recovered = adapter->list_open();
            struct recovered_scope
            {
                std::size_t count = 0;
                std::size_t missing_qty = 0;
                double total_qty = 0.0;
            };
            std::unordered_map<std::string, recovered_scope> per_symbol;
            for (const auto& rb : recovered)
            {
                auto& scope = per_symbol[rb.symbol];
                ++scope.count;
                if (!(rb.qty > 0.0) || !std::isfinite(rb.qty))
                    ++scope.missing_qty;
                else
                    scope.total_qty += rb.qty;
            }
            for (const auto& [symbol, scope] : per_symbol)
            {
                const auto pos = portfolio_.get_positions().find(symbol);
                if (pos == portfolio_.get_positions().end()
                    || std::abs(pos->second.qty) <= 1e-12)
                    throw std::runtime_error(
                        "reconciliation refused startup: resting bracket has no reconciled position for "
                        + symbol);
                const double position_qty = std::abs(pos->second.qty);
                if (scope.missing_qty != 0
                    && (scope.count != 1 || scope.missing_qty != 1))
                    throw std::runtime_error(
                        "reconciliation refused startup: recovered bracket quantities are ambiguous for "
                        + symbol);
                if (scope.missing_qty == 0)
                {
                    const double tolerance = std::max(
                        1e-8, position_qty
                            * config_.reconcile_tolerance_bps / 10000.0);
                    if (std::abs(scope.total_qty - position_qty) > tolerance)
                        throw std::runtime_error(
                            "reconciliation refused startup: recovered bracket quantity contradicts position for "
                            + symbol);
                }
            }
            for (auto& rb : recovered)
            {
                const auto pos = portfolio_.get_positions().find(rb.symbol);
                const auto expected_close_side = pos->second.qty > 0.0
                    ? order_side::sell : order_side::buy;
                if (rb.close_side != expected_close_side)
                    throw std::runtime_error(
                        "reconciliation refused startup: recovered bracket side contradicts position for "
                        + rb.symbol);
                if (!(rb.qty > 0.0) || !std::isfinite(rb.qty))
                {
                    rb.qty = std::abs(pos->second.qty);
                }
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
    // Router is a partial seam; characterized direct submit-result/fill polling
    // remains until the documented decomposition follow-up.
    audit_sink_ = config_.order_audit_sink
        ? config_.order_audit_sink
        : std::make_shared<NoopOrderAuditSink>();
#ifdef HAS_QUESTDB
    if (config_.persist_enabled) {
        if (config_.order_audit_sink)
            throw std::runtime_error(
                "deterministic order audit sink cannot be replaced by QuestDB persistence");
        questdb_begin();  // activation (including sink swap to real QuestdbOrderAuditSink on success) lives inside
    }
#endif

    // OrderIntentProcessor preparatory extraction (see the "OrderIntentProcessor
    // Preparation Report" §6/§7): both leaf collaborators are zero-dependency,
    // so construction order between them and relative to audit_sink_ does not
    // matter — what matters is that attribution_ exists before FillProcessor
    // is constructed below (FillProcessor holds a reference into it).
    attribution_ = std::make_unique<OrderAttributionStore>();
    pending_scheduler_ = std::make_unique<PendingOrderScheduler>();

    // Router wiring (adapters map passed by ref so resolve populates the original execution_adapters_ for iterator compat).
    // See core/docs/internal/engine-decomposition.md (execution router extraction) and engine-decomposition/SKILL.md.
    router_ = std::make_unique<ExecutionRouter>(
        orderbook_registry_,
        config_,
        l2_seeded_symbols_,
        config_.provider.get(),
        execution_adapters_
    );

    // InstrumentSpecCache wiring (PR final). Refs to overrides/provider (cold).
    instrument_spec_cache_ = std::make_unique<InstrumentSpecCache>(
        config_.instrument_overrides, config_.provider.get());

    // Wave 1: create the dashboard builder (cold path extraction).
    // Pass all needed non-owning refs for identical snapshot data + caches.
    dashboard_builder_ = std::make_unique<DashboardSnapshotBuilder>(
        portfolio_,
        order_tracker_,
        risk_manager_,
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
        audit_sink_,
        *pending_scheduler_,
        *attribution_,
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

    // Phase 2 engine decomposition: FillProcessor wiring. Constructed after
    // router_/instrument_spec_cache_/dashboard_builder_/attribution_ (all
    // needed, all valid by this point) and after exit_manager_ (a plain
    // member, already default-constructed). See fills_ member declaration
    // in engine.h for the destruction-order rationale, and
    // core/docs/internal/engine-decomposition.md "Phase 2: Domain
    // Processors" for the full dependency list + callback rationale.
    fills_ = std::make_unique<FillProcessor>(
        portfolio_, order_tracker_, exit_manager_, risk_manager_, adverse_selection_,
        analytics_, audit_sink_, *router_, fill_pool_, *attribution_,
        strategy_, additional_strategies_, additional_strategy_names_,
        primary_strategy_name_, config_,
        dashboard_builder_.get(), shadow_tracker_.get(),
        exchange_portfolio_.has_value() ? &*exchange_portfolio_ : nullptr,
        exchange_analytics_.has_value() ? &*exchange_analytics_ : nullptr,
        static_cast<IEngineHotPathSink&>(*this),
        // unwind_positions moved to OrderIntentProcessor (Phase 1); the
        // IRiskUnwindSink interface replaced the former std::function
        // callback in Phase 2 (see risk_unwind_sink.h). `*this` (engine)
        // implements IRiskUnwindSink privately; its request_unwind()
        // dereferences orders_ only when actually invoked — deep in the
        // event loop, long after orders_ (constructed below) exists. See
        // risk_unwind_sink.h for the full construction-order proof.
        static_cast<IRiskUnwindSink&>(*this)
#ifdef HAS_DEBUG
        , stage_timer_
#endif
    );

    // OrderIntentProcessor wiring. Phase 1 (canonical submit path) +
    // Phase 2 (routing/triggering/exits/pending-due-drain) + Phase 3
    // (cancel/modify/end-of-stream lifecycle) additions. Constructed after
    // every dependency it references — attribution_/pending_scheduler_/
    // router_/fills_/audit_sink_/instrument_spec_cache_/dashboard_builder_/
    // shadow_tracker_/pools/mark-price state — is valid; market_maker_/
    // orderbook_registry_/exit_manager_/execution_adapters_/pause_all_/
    // mm_threaded_/last_sim_time_ are plain engine members, always valid by
    // the time the constructor body runs. pending_stops_/pending_cancels_
    // are no longer passed — OrderIntentProcessor owns them outright as of
    // Phase 3. See order_intent_processor.h for the full dependency list and
    // the Preparation Report's Phase 1/2/3 deliverables for the
    // construction-order rationale.
    orders_ = std::make_unique<OrderIntentProcessor>(
        portfolio_, order_tracker_, risk_manager_, risk_check_.get(),
        analytics_, audit_sink_, *router_, *fills_, *attribution_,
        *pending_scheduler_, exit_manager_, *instrument_spec_cache_,
        market_maker_, orderbook_registry_,
        execution_adapters_,
        order_pool_, rejection_pool_, cancel_pool_, amend_pool_,
        halt_flag_, pause_all_, last_mid_price_, last_mark_symbol_, last_mark_prices_,
        last_mark_prices_mu_, last_sim_time_, last_decision_ts_,
        l2_seeded_symbols_, mm_threaded_,

        dashboard_builder_.get(), shadow_tracker_.get(),
        exchange_portfolio_, config_,
        static_cast<IEngineHotPathSink&>(*this)
    );

    prewarm_object_pools();
    // Direct L2 dispatch is valid before run()/stream setup. Prewarm the
    // bounded same-symbol delay scheduler on the constructor's cold path so
    // its first strategy order is not mistaken for capacity exhaustion.
    clear_pending_state();
    last_mark_symbol_.reserve(32);
    prepare_mark_prices_for_run(/*symbol_hint=*/16);

    // Provider private streams may become ready before engine construction.
    // Retain valid records in the ingress until start_workers(): only then do
    // all configured analytics/logging consumers exist. A pre-latched ingress
    // failure is already terminal and must still refuse construction.
    if (provider_funding_ingress_ && provider_funding_ingress_->failed())
    {
        trigger_halt("provider funding ingress overflow or malformed update");
        throw std::runtime_error("provider funding ingress failed during construction");
    }

    // Constructor rollback safety: callbacks remain disarmed until every
    // potentially-throwing initialization/reconciliation step has completed.
    // A DMS failure before this registration is latched by the provider and
    // delivered synchronously here.
    callbacks_armed_flag_->store(true, std::memory_order_release);
    provider_callbacks_armed_.store(true, std::memory_order_release);
    try
    {
        if (config_.mode == engine_mode::live && config_.provider)
        {
            auto armed_for_halt = callbacks_armed_flag_;
            config_.provider->set_halt_callback(
                [this, armed_for_halt](std::string_view reason) {
                    if (!armed_for_halt
                        || !armed_for_halt->load(std::memory_order_acquire)) return;
                    trigger_halt(reason);
                });
        }
    }
    catch (...)
    {
        // A provider is allowed to retain the callback before reporting a
        // registration failure.  Construction then unwinds without running
        // engine::~engine(), so explicitly disarm every callback that already
        // captured this engine before rethrowing.
        provider_callbacks_armed_.store(false, std::memory_order_release);
        callbacks_armed_flag_->store(false, std::memory_order_release);
        throw;
    }
}

void engine::log_event(const event& ev)
{
    if (config_.is_threaded())
        return;
    if (event_logger_)
    {
        try
        {
            event_logger_->log(ev);
        }
        catch (const std::exception& e)
        {
            run_failed_.store(true, std::memory_order_release);
            trigger_halt(e.what());
        }
        catch (...)
        {
            run_failed_.store(true, std::memory_order_release);
            trigger_halt("durable inline event-log write failed");
        }
    }
}

void engine::publish_event(const event_pointer& ev)
{
    // Opportunistic drain of worker-thread deferred pool returns.
    // This reduces apparent exhaustion and mutex contention on the next
    // acquire when workers release objects between engine acquires.
    // Zero-alloc and safe on hot path (cheap atomic + occasional mutex work).
    drain_object_pool_returns();

    // Phase 2: funding settlements update the primary portfolio cash immediately
    // (advisory for now; later will also feed risk_snapshot / RiskManager).
    if (ev && ev->get_type() == event_type::funding) {
        if (auto* fe = dynamic_cast<funding_event*>(ev.get())) {
            portfolio_.on_funding(*fe);
#ifdef HAS_QUESTDB
            // QuestDB formatting, mutexes and network flushes are cold work.
            // Stage a fixed record here and drain it only at the persistence
            // cadence/finalization boundary; overflow is terminal and loud.
            if (questdb_active_)
            {
                provider_funding_update record;
                record.event_time_ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        fe->get_timestamp().time_since_epoch()).count();
                record.cash_delta = fe->get_cash_delta();
                const auto symbol = fe->get_symbol();
                record.symbol_size = static_cast<std::uint8_t>(symbol.size());
                std::copy(symbol.begin(), symbol.end(), record.symbol.begin());
                if (!funding_audit_ring_.try_push(record))
                    trigger_halt("funding persistence staging queue overflow");
            }
#endif
        }
    }

    // Refresh after event-specific state mutation so a funding settlement is
    // visible in the same snapshot rather than one event late.
    if (dashboard_builder_) dashboard_builder_->refresh_if_due();

    if (config_.threading == thread_preset::inline_mode) {
        return;
    }

    const bool durable_log_required = !config_.event_log_path.empty();

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
            const bool durable_log_drop = durable_log_required
                && std::string_view(name) == "logging";
            if (safety
                && (config_.drop_policy == ring_drop_policy::halt_on_drop
                    || durable_log_drop))
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

    case thread_preset::logging_only:
        TT_PUSH(logging_ring_, logging_drops_, "logging",
                durable_log_required, logging_diag_);
        break;

    case thread_preset::light:
        TT_PUSH(observer_ring_, observer_drops_, "observer", true, observer_diag_);
        break;

    case thread_preset::standard:
        TT_PUSH(logging_ring_,    logging_drops_,    "logging", durable_log_required, logging_diag_);
        TT_PUSH(risk_stats_ring_, risk_stats_drops_, "risk_stats", true,  risk_stats_diag_);
        break;

    case thread_preset::full:
        TT_PUSH(logging_ring_, logging_drops_, "logging", durable_log_required, logging_diag_);
        TT_PUSH(risk_ring_,    risk_drops_,    "risk",    true,  risk_diag_);
        TT_PUSH(stats_ring_,   stats_drops_,   "stats",   false, stats_diag_);
        break;

    case thread_preset::extended:
        TT_PUSH(logging_ring_, logging_drops_, "logging", durable_log_required, logging_diag_);
        TT_PUSH(risk_ring_,    risk_drops_,    "risk",    true,  risk_diag_);
        TT_PUSH(stats_ring_,   stats_drops_,   "stats",   false, stats_diag_);
        TT_PUSH(mm_ring_,      mm_drops_,      "mm",      false, mm_diag_);
        break;
    }

#undef TT_PUSH
}

void engine::request_unwind(std::size_t& event_count)
{
    if (!orders_)
    {
        // FillProcessor normally calls this only after construction. If that
        // invariant is ever broken, latch the same terminal reason its caller
        // uses instead of silently skipping the emergency unwind.
        trigger_halt("risk post-fill limit breached - engine halted");
        return;
    }
    orders_->unwind_positions(event_count);
}

void engine::trigger_halt(std::string_view reason) noexcept
{
    if (halt_flag_.exchange(true, std::memory_order_acq_rel))
        return;

    run_failed_.store(true, std::memory_order_release);
    if (config_.provider)
    {
        try
        {
            if (auto transport = config_.provider->get_transport())
                transport->request_stop();
        }
        catch (...) {}
    }

    if (auto* dash = config_.dashboard.get())
    {
        dash->stats().halt_flag.store(true, std::memory_order_release);
        // Halt publication is safety state; diagnostics are best-effort and
        // must never escape a provider/DMS callback thread.
        try { dash->set_state(truetest::ui::connection_state::halted); }
        catch (...) {}
        try { dash->set_shutdown_reason(reason); }
        catch (...) {}
        try {
            dash->push_event(truetest::ui::event_severity::error, reason);
        }
        catch (...) {}
    }
    else
    {
        try { std::cerr << "  ! engine halt — " << reason << "\n"; }
        catch (...) {}
    }
}

bool engine::request_operator_kill(std::chrono::milliseconds deadline)
{
    trigger_halt("operator kill requested");
    if (!config_.live_safety_session) return false;
    const auto report = config_.live_safety_session->shutdown_once(
        live_shutdown_reason::operator_kill, deadline);
    return report.quiesce_succeeded
        && report.kill_succeeded && report.provider_closed;
}

bool engine::finalize_live_shutdown(live_shutdown_reason reason)
{
    if (!config_.live_safety_session) return true;

    const auto report = config_.live_safety_session->shutdown_once(reason);
    if (config_.mode != engine_mode::live)
        return true;

    if (report.quiesce_succeeded
        && report.kill_succeeded && report.provider_closed)
        return true;

    if (!live_shutdown_failure_reported_.exchange(
            true, std::memory_order_acq_rel))
    {
        trigger_halt(!report.quiesce_succeeded
            ? "live provider quiesce failed or remained ambiguous"
            : (report.kill_succeeded
                ? "live provider shutdown did not finish"
                : "live kill failed or remained ambiguous"));
        std::cerr << "  WARNING: live shutdown was incomplete; process remains "
                     "halted and venue safety must be verified manually.\n";
    }
    return false;
}

// Dashboard methods moved to DashboardSnapshotBuilder (Wave 1).
// Old implementations removed; calls updated to delegate via dashboard_builder_.

// (stray build_dashboard_view body excised)

// Wave 2 skeleton helpers (setup/teardown, pending drain, paper tape, marks)
// live in engine_pending.cpp to keep freeze-surface engine.cpp from sprawling.

engine::~engine()
{
    // Ensure workers are joined and provider resources (incl. any
    // lingering transport threads and callbacks) are torn down before
    // our members (pools, rings, exit_manager, etc.) are destroyed.
    stop_workers();
    // Inline persistence has no logging worker to finalize it.  Provider
    // shutdown above may have published a last funding settlement, so the
    // ledger trailer must be written only after that final drain.
    finalize_inline_event_log();

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


// See declaration in engine.h for documentation.

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

    prepare_event_logging();

#ifdef HAS_DEBUG
    memory_sampler_.set_start(debug::memory_snapshot::capture());
#endif

    setup_event_loop_infra();

    const auto base_ts = (config_.mode == engine_mode::backtest)
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
            bar.volume,
            bar.quantity_scale
        );
        const double bar_volume = tt::quantity_scale::to_base(
            mkt.get_volume(), mkt.get_quantity_scale());
        mkt.set_recv_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        auto sim_time = mkt.get_timestamp();
        const auto& symbol = mkt.get_symbol();
        last_sim_time_ = sim_time;
        // F-08: the bar's timestamp is its OPEN; a strategy reacting to its
        // close decides one interval later. Carry that instant alongside.
        last_decision_ts_ = sim_time + bar_interval_;


        last_mid_price_.store(mkt.get_open(), std::memory_order_release);
        last_mark_symbol_ = symbol;
        { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[symbol] = mark_point{mkt.get_open(), mkt.get_timestamp()}; }

        if (flatten_request_.exchange(false, std::memory_order_acq_rel))
            orders_->unwind_positions(event_count);

        // Advance adapter clocks so latency-gated cancels complete offline.
        if (router_) router_->advance_all(sim_time);

        {
            DEBUG_STAGE(stage_timer_, pending_drain);
            // Per-order mid + book re-center (open mark already stored above).
            orders_->drain_due(sim_time, event_count, halt_requested, symbol);
        }
        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

        last_mid_price_.store(mkt.get_close(), std::memory_order_release);
        last_mark_symbol_ = symbol;
        { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[symbol] = mark_point{mkt.get_close(), mkt.get_timestamp()}; }

        {
            DEBUG_STAGE(stage_timer_, stop_check);
            orders_->check_pending_stops(symbol, mkt.get_open(), mkt.get_high(),
                                mkt.get_low(), sim_time, event_count,
                                halt_requested);
        }
        const double swept_volume = orders_->sweep_resting_limits(
            symbol, mkt.get_low(), mkt.get_high(), sim_time,
            event_count, halt_requested, bar_volume);

        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

        feed_paper_trade_and_drain(symbol, mkt.get_close(),
                                   std::max(0.0, bar_volume - swept_volume),
                                   std::nullopt,
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
            orders_->drain_async_submit_results(provider_adapter.get());
            if (provider_adapter && config_.mode != engine_mode::shadow)
            {
                (void)fills_->process_adapter_fills(
                    provider_adapter, event_count, halt_requested);
            }
            else if (provider_adapter)
            {
                (void)fills_->process_shadow_exchange_fills(
                    provider_adapter, halt_requested);
            }
        }
        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

        {
            DEBUG_STAGE(stage_timer_, mm_replenish);
            auto ob = orderbook_registry_.get_or_create(symbol);
            if (!mm_threaded_ &&
                !l2_seeded_symbols_.count(symbol))
            {
                auto mm_trades = market_maker_.replenish(
                    ob, last_mid_price_.load(std::memory_order_relaxed));
                orders_->deliver_mm_book_trades(symbol, mm_trades, sim_time,
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
                        static_cast<quantity>(std::round(
                            mm_order.get_quantity() * config_.qty_scale)));
                    auto mm_trades = mm_ob->add_synthetic_order(mm_ob_order);
                    orders_->deliver_mm_book_trades(mm_order.get_symbol(), mm_trades,
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
        if (!config_.has_async_analytics())
            analytics_.on_event(mkt_ptr);
        else
            analytics_.on_mark(symbol, mkt.get_close());
        event_count++;

        // Canonical: exits before strategy decision (matches tick path).
        if (orders_->evaluate_exits(symbol,
                           mkt.get_open(), mkt.get_low(), mkt.get_high(), mkt.get_close(),
                           sim_time, event_count, mkt.get_recv_ns()))
            break;

        if (!strategy_ || halt_flag_.load(std::memory_order_acquire))
            break;

        std::optional<order_event> order_opt;
        {
            DEBUG_STAGE(stage_timer_, strategy);
            sync_strategy_account_equity(*strategy_);
            order_opt = strategy_->on_market(mkt);
        }

        if (order_opt)
        {
            order_opt->set_recv_ns(mkt.get_recv_ns());
            if (!primary_strategy_name_.empty())
                order_opt->set_strategy_name(primary_strategy_name_);
            const double pre_net = !primary_strategy_name_.empty()
                ? portfolio_.get_strategy_position_qty(primary_strategy_name_, order_opt->get_symbol())
                : (portfolio_.get_positions().count(order_opt->get_symbol())
                    ? portfolio_.get_positions().at(order_opt->get_symbol()).qty : 0.0);
            orders_->route(*order_opt, sim_time, event_count, halt_requested);
            orders_->finalize_route(*strategy_, primary_strategy_name_, *order_opt,
                                    halt_requested, pre_net);
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

    orders_->finalize_end_of_stream(event_count, halt_requested);

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
