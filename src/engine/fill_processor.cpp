// FillProcessor implementation — Phase 2 engine decomposition (2026-08).
// Moved verbatim (behavior-preserving) from src/engine/engine_fills.cpp
// (handle_engine_fill, stamp_fill_attribution, dispatch_fill_to_strategy,
// process_adapter_fills) and src/engine/engine_orders.cpp
// (notify_position_change_all). See core/docs/internal/engine-decomposition.md
// "Phase 2: Domain Processors" for the call-site mapping and ordering evidence.
//
// LIVE-SAFETY SURFACE: see scripts/check-live-safety-freeze.sh.
#include "fill_processor.h"

#include "dashboard_snapshot_builder.h"
#include "analytics/shadow_tracker.h"
#include "execution/execution_adapter.h"
#include "types/pool_exhausted.h"

#include "debug/stage_timer.h"

#include <utility>

FillProcessor::FillProcessor(
    portfolio& portfolio,
    OrderTracker& order_tracker,
    truetest::exits::ExitManager& exit_manager,
    RiskManager& risk_manager,
    AdverseSelectionTracker& adverse_selection,
    Analytics& analytics,
    const std::shared_ptr<IOrderAuditSink>& audit_sink,
    ExecutionRouter& router,
    ObjectPool<fill_event>& fill_pool,
    const OrderAttributionStore& attribution,
    std::shared_ptr<IStrategy>& strategy,
    std::vector<std::shared_ptr<IStrategy>>& additional_strategies,
    std::vector<std::string>& additional_strategy_names,
    const std::string& primary_strategy_name,
    const engine_config& config,
    DashboardSnapshotBuilder* dashboard_builder,
    ShadowTracker* shadow_tracker,
    ::portfolio* exchange_portfolio,
    Analytics* exchange_analytics,
    IEngineHotPathSink& hotpath,
    IRiskUnwindSink& risk_unwind_sink
#ifdef HAS_DEBUG
    , debug::StageTimer& stage_timer
#endif
    )
    : portfolio_(portfolio), order_tracker_(order_tracker), exit_manager_(exit_manager),
      risk_manager_(risk_manager), adverse_selection_(adverse_selection), analytics_(analytics),
      audit_sink_(audit_sink), router_(router), fill_pool_(fill_pool), attribution_(attribution),
      strategy_(strategy), additional_strategies_(additional_strategies),
      additional_strategy_names_(additional_strategy_names),
      primary_strategy_name_(primary_strategy_name), config_(config),
      dashboard_builder_(dashboard_builder), shadow_tracker_(shadow_tracker),
      exchange_portfolio_(exchange_portfolio),
      exchange_analytics_(exchange_analytics),
      hotpath_(hotpath), risk_unwind_sink_(risk_unwind_sink)
#ifdef HAS_DEBUG
      , stage_timer_(stage_timer)
#endif
{
}

bool FillProcessor::register_shadow_order(const order_event& order)
{
    if (config_.mode != engine_mode::shadow
        || !exchange_portfolio_ || !exchange_analytics_)
    {
        hotpath_.trigger_halt(
            "shadow exchange order registration lacks independent ledgers");
        return false;
    }
    if (!shadow_exchange_order_tracker_.register_order(order))
    {
        hotpath_.trigger_halt(
            "shadow exchange order registration failed before submission");
        return false;
    }
    shadow_exchange_order_tracker_.set_status(
        order.get_order_id(), order_status::pending);
    return true;
}

void FillProcessor::stamp_shadow_fill_attribution(fill_event& fill) const
{
    if (fill.get_strategy_name().empty())
    {
        const auto& strategy_name = lookup_strategy_name(fill.get_order_id());
        if (!strategy_name.empty())
            fill.set_strategy_name(strategy_name);
    }
    if (fill.get_opener_order_id() == 0)
    {
        if (const auto opener = lookup_opener(fill.get_order_id()); opener != 0)
            fill.set_opener_order_id(opener);
    }

    // Divergence can make the shadow exchange lots differ from the simulated
    // portfolio. Resolve an implicit close only against the ledger it will
    // actually mutate; consulting portfolio_ here would cross-contaminate the
    // two books and can attribute a venue fill to a nonexistent shadow lot.
    if (exchange_portfolio_
        && fill.get_opener_order_id() == fill.get_order_id())
    {
        std::uint64_t candidate = 0;
        bool ambiguous = false;
        for (const auto& [id, lot] : exchange_portfolio_->get_lots())
        {
            if (lot.symbol != fill.get_symbol() || lot.side == fill.get_side())
                continue;
            if (!fill.get_strategy_name().empty()
                && lot.strategy_name != fill.get_strategy_name())
                continue;
            if (candidate != 0)
            {
                ambiguous = true;
                break;
            }
            candidate = id;
        }
        if (!ambiguous && candidate != 0)
            fill.set_opener_order_id(candidate);
    }
}

bool FillProcessor::ingest_shadow_exchange_fill(
    fill_event& fill, bool& halt_requested,
    bool* delivery_consumed, IExecutionAdapter* delivery_adapter)
{
    if (delivery_consumed)
        *delivery_consumed = false;
    const auto consume_delivery = [&]() -> bool
    {
        if (delivery_adapter
            && !delivery_adapter->acknowledge_fill(fill.get_fill_id()))
        {
            hotpath_.trigger_halt(
                "shadow exchange fill acknowledgement failed");
            halt_requested = true;
            return false;
        }
        if (delivery_consumed)
            *delivery_consumed = true;
        return true;
    };

    if (config_.mode != engine_mode::shadow
        || !exchange_portfolio_ || !exchange_analytics_ || !shadow_tracker_)
    {
        hotpath_.trigger_halt("shadow exchange fill lacks independent ledgers");
        halt_requested = true;
        return false;
    }

    const auto validation = shadow_exchange_order_tracker_.validate_fill(
        fill, /*require_exchange_identity=*/true,
        /*require_fill_identity=*/true);
    if (validation.idempotent_noop())
        return consume_delivery();
    if (validation.rejected())
    {
        audit_sink_->record_event(
            "shadow_order_lifecycle", fill.get_symbol().c_str(), "",
            fill.get_order_id(), to_string(validation.code),
            "shadow exchange fill failed canonical admission", "{}");
        hotpath_.trigger_halt(
            "shadow exchange fill failed canonical admission - reconciliation required");
        halt_requested = true;
        return false;
    }

    stamp_shadow_fill_attribution(fill);
    auto fill_ptr = acquire_pooled_fill(fill);
    if (!shadow_exchange_order_tracker_.commit_fill(fill, validation))
    {
        hotpath_.trigger_halt(
            "validated shadow exchange fill commit failed - reconciliation required");
        halt_requested = true;
        return false;
    }

    fill.set_economic_quantity(validation.economic_quantity);
    fill_ptr->set_economic_quantity(validation.economic_quantity);
    exchange_portfolio_->on_fill(
        fill, fill.get_opener_order_id(), fill.get_strategy_name());
    exchange_analytics_->on_event(fill_ptr);
    shadow_tracker_->on_exchange_fill(fill);

    return consume_delivery();
}

bool FillProcessor::process_shadow_exchange_fills(
    const std::shared_ptr<IExecutionAdapter>& adapter,
    bool& halt_requested)
{
    if (!adapter)
        return true;

    if (adapter->supports_transactional_fill_delivery())
    {
        fill_event fill({}, "", 0, order_side::buy, 0.0, 0.0);
        while (adapter->peek_fill(fill))
        {
            bool consumed = false;
            if (!ingest_shadow_exchange_fill(
                    fill, halt_requested, &consumed, adapter.get()))
                return false;
            if (!consumed)
            {
                hotpath_.trigger_halt(
                    "shadow fill pipeline returned without consuming delivery");
                halt_requested = true;
                return false;
            }
        }
        return true;
    }

    std::vector<fill_event> fills;
    if (!router_.poll_fills(adapter.get(), fills))
        return true;
    for (auto& fill : fills)
    {
        if (!ingest_shadow_exchange_fill(fill, halt_requested))
            return false;
    }
    return true;
}

uint64_t FillProcessor::lookup_opener(uint64_t order_id) const
{
    return attribution_.opener_for(order_id);
}

const std::string& FillProcessor::lookup_strategy_name(uint64_t order_id) const
{
    return attribution_.strategy_for(order_id);
}

std::shared_ptr<fill_event> FillProcessor::acquire_pooled_fill(const fill_event& f)
{
    try
    {
        return fill_pool_.acquire(f);
    }
    catch (const pool_exhausted& e)
    {
        hotpath_.trigger_halt(e.what());
        throw;
    }
}

bool FillProcessor::process_adapter_fills(const std::shared_ptr<IExecutionAdapter>& adapter,
                                          std::size_t& event_count, bool& halt_requested)
{
    return process_adapter_fills(
        adapter, event_count, halt_requested,
        config_.mode == engine_mode::shadow
            ? fill_context::shadow_simulated
            : fill_context::primary);
}

bool FillProcessor::process_adapter_fills(const std::shared_ptr<IExecutionAdapter>& adapter,
                                          std::size_t& event_count,
                                          bool& halt_requested,
                                          fill_context context)
{
    if (!adapter)
        return true;
    const char* status_reason = context == fill_context::risk_unwind
        ? "risk_unwind"
        : nullptr;

    if (adapter->supports_transactional_fill_delivery())
    {
        fill_event fill({}, "", 0, order_side::buy, 0.0, 0.0);
        while (adapter->peek_fill(fill))
        {
#ifdef HAS_DEBUG
            DEBUG_STAGE(stage_timer_, fill_processing);
#endif
            bool delivery_consumed = false;
            const bool keep_running = ingest(
                fill, event_count, halt_requested, context, status_reason,
                &delivery_consumed, adapter.get());
            if (!keep_running)
                return false;
            if (!delivery_consumed)
            {
                hotpath_.trigger_halt(
                    "fill pipeline returned without consuming or rejecting delivery");
                halt_requested = true;
                return false;
            }
        }
        return true;
    }

    if (config_.mode == engine_mode::live)
    {
        hotpath_.trigger_halt(
            "live execution adapter lacks transactional fill delivery");
        halt_requested = true;
        return false;
    }

    std::vector<fill_event> fills;
    if (!router_.poll_fills(adapter.get(), fills))
        return true;

#ifdef HAS_DEBUG
    DEBUG_STAGE(stage_timer_, fill_processing);
#endif
    for (auto& f : fills)
    {
        if (!ingest(f, event_count, halt_requested, context, status_reason))
            return false;
    }
    return true;
}

void FillProcessor::stamp_fill_attribution(fill_event& f) const
{
    // Phase 1 deepdive: ensure every fill carries opener + strategy for
    // consistent per-lot bookkeeping across portfolio, ExitManager, QuestDB,
    // workers (via rings), shadow duals, analytics, and dashboard snapshot.
    if (f.get_strategy_name().empty())
    {
        const auto& sn = lookup_strategy_name(f.get_order_id());
        if (!sn.empty())
            f.set_strategy_name(sn);
    }

    if (f.get_opener_order_id() == 0)
    {
        if (auto op = lookup_opener(f.get_order_id()); op != 0)
            f.set_opener_order_id(op);
    }

    // Legacy/single-lot strategies often emit an ordinary opposite-side
    // order without an explicit opener id. The metadata fallback above maps
    // such an order to itself, which would leave the old lot/bracket orphaned
    // on a close or flip. Resolve the unambiguous opposing lot here. Multi-lot
    // strategies remain required to stamp an opener explicitly.
    if (f.get_opener_order_id() == f.get_order_id())
    {
        std::uint64_t candidate = 0;
        bool ambiguous = false;
        for (const auto& [id, lot] : portfolio_.get_lots())
        {
            if (lot.symbol != f.get_symbol() || lot.side == f.get_side())
                continue;
            if (!f.get_strategy_name().empty()
                && lot.strategy_name != f.get_strategy_name())
                continue;
            if (candidate != 0)
            {
                ambiguous = true;
                break;
            }
            candidate = id;
        }
        if (!ambiguous && candidate != 0)
            f.set_opener_order_id(candidate);
    }
}

bool FillProcessor::ingest(fill_event& f,
                           std::size_t& event_count,
                           bool& halt_requested,
                           fill_context context,
                           const char* status_reason,
                           bool* delivery_consumed,
                           IExecutionAdapter* delivery_adapter)
{
    if (delivery_consumed)
        *delivery_consumed = false;
    const auto consume_delivery = [&]() -> bool
    {
        if (delivery_adapter
            && !delivery_adapter->acknowledge_fill(f.get_fill_id()))
        {
            hotpath_.trigger_halt(
                "transactional fill delivery acknowledgement failed");
            halt_requested = true;
            return false;
        }
        if (delivery_consumed)
            *delivery_consumed = true;
        return true;
    };
    const bool authoritative_replay =
        context == fill_context::authoritative_replay;
    const bool run_post_fill_risk =
        context == fill_context::primary
        || context == fill_context::shadow_simulated;
    const bool mark_shadow_sim =
        context == fill_context::shadow_simulated;
    // The authoritative ledger performs the first, read-only admission pass.
    // It must run before attribution, protective-ticket checks, pool use, or
    // any economic mutation: an old reconnect replay can legitimately arrive
    // after its lot and protection were retired, while a malformed/unknown
    // fill must leave every economic subsystem untouched.
    const auto fill_validation = order_tracker_.validate_fill(
        f, config_.mode == engine_mode::live,
        /*require_fill_identity=*/true);
    if (fill_validation.idempotent_noop())
    {
        audit_sink_->record_event(
            "order_lifecycle", f.get_symbol().c_str(), "",
            f.get_order_id(), to_string(fill_validation.code),
            "economic fill already represented by the cumulative ledger cursor - ignored",
            "{}");
        return consume_delivery();
    }
    if (fill_validation.rejected())
    {
        audit_sink_->record_event(
            "order_lifecycle", f.get_symbol().c_str(), "",
            f.get_order_id(), to_string(fill_validation.code),
            "economic fill failed canonical admission; reconciliation required",
            "{}");
        hotpath_.trigger_halt("economic fill failed canonical admission - reconciliation required");
        halt_requested = true;
        return false;
    }

    // Keep the venue/strategy-provided attribution distinct from the later
    // single-lot fallback. A deliberate attributed close is a close-only
    // contract: allowing it to exceed that lot would manufacture a new,
    // unprotected position under the closer id. Ordinary implicit flip orders
    // enter with self attribution and remain eligible for scalar flip handling.
    uint64_t reported_opener = f.get_opener_order_id();
    if (!authoritative_replay && reported_opener == 0)
        reported_opener = lookup_opener(f.get_order_id());
    if (!authoritative_replay)
        stamp_fill_attribution(f);

    const uint64_t opener = f.get_opener_order_id();
    const std::string& strat = f.get_strategy_name();
    struct protective_lifecycle_snapshot
    {
        std::uint64_t opener_order_id;
        double requested_qty;
        double filled_qty;
        order_exit_reason reason;
    };
    const auto protective_before = [&]()
        -> std::optional<protective_lifecycle_snapshot>
    {
        const auto protective =
            exit_manager_.protective_exit_for_order(f.get_order_id());
        if (!protective) return std::nullopt;
        return protective_lifecycle_snapshot{
            protective->opener_order_id, protective->requested_qty,
            protective->filled_qty, protective->reason};
    }();

    // ExitManager::on_fill below can erase a fully consumed protection.
    // Keep only copied scalars across that mutation; lifecycle text comes
    // from this already-attributed fill, never from a dangling ticket view.

    // Multi-lot strategies own their attribution. A bare opposite-side order
    // with more than one eligible lot has no deterministic close allocation;
    // applying it as a self-opener would leave orphaned lots/brackets while
    // changing the net position. Refuse it before the authoritative ledger.
    if (!authoritative_replay && opener == f.get_order_id())
    {
        std::size_t opposing_lots = 0;
        for (const auto& [_, lot] : portfolio_.get_lots())
        {
            if (lot.symbol != f.get_symbol() || lot.side == f.get_side())
                continue;
            if (!strat.empty() && lot.strategy_name != strat)
                continue;
            if (++opposing_lots > 1)
                break;
        }
        if (opposing_lots > 1)
        {
            audit_sink_->record_event(
                "order_lifecycle", f.get_symbol().c_str(), strat.c_str(),
                f.get_order_id(), "ambiguous_implicit_close",
                "implicit opposite-side fill has multiple eligible lots", "{}");
            hotpath_.trigger_halt("implicit opposite-side fill has ambiguous multi-lot attribution");
            halt_requested = true;
            return false;
        }
    }

    // A known protective close has its own stale/oversize policy below: the
    // deterministic backtest can suppress a proven-stale ticket, whereas
    // shadow/live enter a terminal reconciliation state. All other explicit
    // closers are close-only contracts and may not reverse their referenced
    // lot into new unprotected exposure.
    if (!authoritative_replay
        && !exit_manager_.is_known_protective_close(f.get_order_id()) &&
        reported_opener != 0 && reported_opener != f.get_order_id())
    {
        const auto lot = portfolio_.get_lots().find(reported_opener);
        const bool admissible_explicit_close =
            lot != portfolio_.get_lots().end()
            && lot->second.symbol == f.get_symbol()
            && lot->second.side != f.get_side()
            && fill_validation.economic_quantity
                <= lot->second.qty_open + 1e-12;
        if (!admissible_explicit_close)
        {
            audit_sink_->record_event(
                "order_lifecycle", f.get_symbol().c_str(), strat.c_str(),
                f.get_order_id(), "explicit_close_oversize",
                "explicit attributed close is stale or exceeds its referenced lot", "{}");
            hotpath_.trigger_halt("explicit attributed close is stale or exceeds its referenced lot");
            halt_requested = true;
            return false;
        }
    }

    // A close is only admissible while its attributed lot still exists and,
    // for a ticketed protective exit, while it cannot exceed its reserved
    // remainder.  Applying an excess/late close through portfolio would turn
    // the residual into a fresh opposite lot.  The venue may already have
    // changed state, so this is a terminal reconciliation condition, never
    // an opportunity to retry or silently discard it.
    if (!authoritative_replay
        && opener != 0 && opener != f.get_order_id() &&
        exit_manager_.is_known_protective_close(f.get_order_id()))
    {
        const auto lot = portfolio_.get_lots().find(opener);
        const bool lot_is_live = lot != portfolio_.get_lots().end()
            && lot->second.symbol == f.get_symbol()
            && lot->second.side != f.get_side();
        if (!lot_is_live ||
            !exit_manager_.protective_fill_is_admissible(
                f.get_order_id(), fill_validation.economic_quantity))
        {
            if (config_.mode == engine_mode::backtest)
            {
                // A deterministic simulator can prove this close is stale
                // from the local lot ledger. Suppress it and terminally retire
                // the ticket instead of manufacturing a reversal. Live/shadow
                // exchange reports remain a loud reconciliation emergency.
                (void)exit_manager_.on_protective_close_terminal(
                    f.get_order_id());
                order_tracker_.set_status(f.get_order_id(), order_status::cancelled);
                audit_sink_->record_event(
                    "exit_lifecycle", f.get_symbol().c_str(), strat.c_str(),
                    f.get_order_id(), "stale_suppressed",
                    "simulated stale protective close suppressed before reversal", "{}");
                return consume_delivery();
            }
            audit_sink_->record_event(
                "exit_lifecycle", f.get_symbol().c_str(), strat.c_str(),
                f.get_order_id(), "terminal_emergency",
                "late or over-sized attributed close would reverse a flat lot", "{}");
            hotpath_.trigger_halt("late or over-sized attributed close would reverse a flat lot");
            halt_requested = true;
            return false;
        }
    }

    // Reserve every fallible pooled resource before the ledger commit point.
    // Pool exhaustion therefore cannot leave the order ledger ahead of the
    // portfolio, exits, risk, analytics, or reporting state.
    auto fill_ptr = acquire_pooled_fill(f);

    // R3: this is the single economic commit point. The engine loop is the
    // sole writer, so the read-only validation above cannot become stale.
    // A failed commit here is an internal invariant violation, not a duplicate.
    if (!order_tracker_.commit_fill(f, fill_validation))
    {
        audit_sink_->record_event(
            "order_lifecycle", f.get_symbol().c_str(), "",
            f.get_order_id(), "fill_commit_fault",
            "validated economic fill could not be committed; reconciliation required",
            "{}");
        hotpath_.trigger_halt("validated economic fill commit failed - reconciliation required");
        halt_requested = true;
        return false;
    }

    // The durable replay record retains the raw venue execution exactly as
    // it was fingerprinted by OrderTracker. Replaying that record will derive
    // the same canonical cursor delta again; a later reconnect replay of the
    // original venue payload therefore remains an exact native-ID duplicate.
    if (!authoritative_replay)
        hotpath_.log_event(f);

    // All downstream ledgers must book the exact same economic quantity as
    // OrderTracker. Raw venue slices such as 0.2 can differ by one ULP from
    // cumulative(0.3)-previous(0.1); retaining the raw slice would make the
    // physical position diverge from the authoritative filled cursor.
    f.set_economic_quantity(fill_validation.economic_quantity);
    fill_ptr->set_economic_quantity(fill_validation.economic_quantity);
    const auto new_status = order_tracker_.get_order_status(f.get_order_id());
    if (dashboard_builder_) {
        dashboard_builder_->cache_fill(f);
        if (new_status == order_status::partially_filled)
            dashboard_builder_->update_open_order_status(f.get_order_id(), "partial");
        else
            dashboard_builder_->erase_open_order(f.get_order_id());
    }
    const double fill_qty = f.get_filled_quantity();
    bool is_flip = false;
    double close_qty = 0.0;
    double open_qty = 0.0;
    uint64_t close_opener = opener;

    const auto& positions = portfolio_.get_positions();
    auto pos_it = positions.find(f.get_symbol());
    if (pos_it != positions.end() && fill_qty > 1e-12)
    {
        const double pos_qty = pos_it->second.qty;
        if (pos_qty > 1e-12 && f.get_side() == order_side::sell && fill_qty > pos_qty + 1e-12)
        {
            is_flip = true;
            close_qty = pos_qty;
            open_qty = fill_qty - pos_qty;
        }
        else if (pos_qty < -1e-12 && f.get_side() == order_side::buy && fill_qty > (-pos_qty) + 1e-12)
        {
            is_flip = true;
            close_qty = -pos_qty;
            open_qty = fill_qty - (-pos_qty);
        }
    }

    // The physical execution is the sole canonical event for the ledger,
    // strategy, audit, publish, analytics, and replay paths. A crossing fill
    // has two bracket-accounting effects only: release the closed opener and
    // arm protection for its residual new exposure.
    portfolio_.on_fill(f, opener, strat);
    if (!authoritative_replay)
    {
        if (is_flip)
        {
            exit_manager_.on_fill(f, close_opener, close_qty);
            exit_manager_.on_fill(f, f.get_order_id(), open_qty);
        }
        else
        {
            exit_manager_.on_fill(f, opener);
        }
        risk_manager_.on_fill(f);
    }
    const char* src =
        (f.get_source() == fill_source::exchange)  ? "exchange"
      : (f.get_source() == fill_source::simulated) ? "simulated"
      :                                              "local";
    if (!authoritative_replay)
        audit_sink_->record_fill(f, opener, strat.c_str(), src);
    hotpath_.publish_event(fill_ptr);
    analytics_.on_event(fill_ptr);

    if (mark_shadow_sim && config_.mode == engine_mode::shadow && shadow_tracker_)
        shadow_tracker_->on_simulated_fill(f);

    if (!authoritative_replay)
    {
        audit_sink_->record_status_transition(f.get_order_id(),
            fill_validation.before, new_status, status_reason);
        if (protective_before)
        {
            const double filled = std::min(protective_before->requested_qty,
                protective_before->filled_qty + f.get_filled_quantity());
            audit_sink_->record_exit_lifecycle(exit_lifecycle_record{
                f.get_order_id(), f.get_order_id(), protective_before->opener_order_id,
                f.get_fill_id(), {}, {}, {}, f.get_timestamp(),
                protective_before->requested_qty, filled,
                std::max(0.0, protective_before->requested_qty - filled),
                protective_before->reason, order_status::open, new_status,
                f.get_symbol().c_str(), strat.c_str(), "fill", "filled"});
        }
        sweep_flat_brackets_if_needed(
            f.get_symbol(), portfolio_.position_open(f.get_symbol()));
        ++event_count;
    }

    // Everything that constitutes the economic event (ledger, portfolio,
    // exits/risk state, audit, publication and analytics) is now complete.
    // A post-fill risk halt below must still ACK this delivery; replaying it
    // would otherwise attempt to book an already-applied execution again.
    if (!consume_delivery())
        return false;

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
            }
            else
            {
                // Unwind (order-pipeline emergency liquidation, owned by
                // OrderIntentProcessor as of Phase 2) must run BEFORE
                // trigger_halt: unwind_positions still needs the router.
                if (config_.risk_unwind)
                    risk_unwind_sink_.request_unwind(event_count);
                hotpath_.trigger_halt(
                    "risk post-fill limit breached - engine halted");
                halt_requested = true;
                return false;
            }
        }
    }

    // Strategies and observational analytics are fallible user callbacks,
    // not economic ledgers. Invoke them exactly once only after the canonical
    // fill, publication, analytics and transactional delivery ACK are all
    // complete. A callback fault cannot be retried safely, so halt without
    // rolling back or replaying the already committed execution.
    if (!authoritative_replay)
    {
        try
        {
            dispatch_fill_to_strategy(f);
            adverse_selection_.on_fill(f);
            notify_position_change_all(
                f.get_symbol(), portfolio_.position_open(f.get_symbol()),
                /*sweep_flat_brackets=*/false);
        }
        catch (...)
        {
            hotpath_.trigger_halt(
                "post-commit fill observer failed - execution remains committed");
            halt_requested = true;
            return false;
        }
    }
    return true;
}

void FillProcessor::dispatch_fill_to_strategy(const fill_event& f) const
{
    const std::string& name = lookup_strategy_name(f.get_order_id());
    const std::uint64_t opener = lookup_opener(f.get_order_id());

    // BF-15: reserve distinguishable sentinels and skip strategy dispatch.
    if (name == "__engine_unwind__" || name == "risk_unwind" ||
        f.get_strategy_name() == "__engine_unwind__" || f.get_strategy_name() == "risk_unwind")
    {
        return;
    }

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

void FillProcessor::notify_position_change_all(const std::string& symbol, bool open,
                                               bool sweep_flat_brackets)
{
    if (sweep_flat_brackets)
        sweep_flat_brackets_if_needed(symbol, open);

    // Legacy net-truth push for strategies that still override
    // set_position_open. Multi-lot strategies ignore this and track
    // openers via on_fill.
    if (strategy_) strategy_->set_position_open(symbol, open);
    for (auto& s : additional_strategies_)
        if (s) s->set_position_open(symbol, open);
}

void FillProcessor::sweep_flat_brackets_if_needed(
    const std::string& symbol, bool position_open)
{
    // On a net-flat transition, sweep any leftover bracket for a
    // single-lot strategy. This canonical ExitManager mutation remains before
    // the delivery ACK; fallible strategy callbacks are invoked afterwards.
    if (!position_open)
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
