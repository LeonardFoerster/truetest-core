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
    IOrderAuditSink& audit_sink,
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
    std::function<void(const event&)> log_event,
    std::function<void(const event_pointer&)> publish_event,
    std::function<void(std::string_view)> trigger_halt,
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
      log_event_(std::move(log_event)), publish_event_(std::move(publish_event)),
      trigger_halt_(std::move(trigger_halt)), risk_unwind_sink_(risk_unwind_sink)
#ifdef HAS_DEBUG
      , stage_timer_(stage_timer)
#endif
{
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
        trigger_halt_(e.what());
        throw;
    }
}

bool FillProcessor::process_adapter_fills(const std::shared_ptr<IExecutionAdapter>& adapter,
                                          std::size_t& event_count, bool& halt_requested)
{
    if (!adapter)
        return true;

    std::vector<fill_event> fills;
    if (!router_.poll_fills(adapter.get(), fills))
        return true;

#ifdef HAS_DEBUG
    DEBUG_STAGE(stage_timer_, fill_processing);
#endif
    const bool mark_sim = (config_.mode == engine_mode::shadow);
    for (auto& f : fills)
    {
        if (!handle_fill(f, event_count, halt_requested,
                         /*run_post_fill_risk=*/true,
                         /*mark_shadow_sim=*/mark_sim))
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

bool FillProcessor::handle_fill(fill_event& f,
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
    auto fill_ptr = acquire_pooled_fill(f);
    log_event_(f);
    portfolio_.on_fill(f, opener, strat);
    dispatch_fill_to_strategy(f);
    adverse_selection_.on_fill(f);
    exit_manager_.on_fill(f, opener);
    risk_manager_.on_fill(f);
    const char* src =
        (f.get_source() == fill_source::exchange)  ? "exchange"
      : (f.get_source() == fill_source::simulated) ? "simulated"
      :                                              "local";
    audit_sink_.record_fill(f, opener, strat.c_str(), src);
    audit_sink_.record_status_transition(f.get_order_id(),
        order_status::open, new_status, status_reason);
    notify_position_change_all(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
    publish_event_(fill_ptr);
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
                audit_sink_.record_event(
                    "risk_decision",
                    f.get_symbol().c_str(),
                    "",
                    f.get_order_id(),
                    "soft_post_fill",
                    "post-fill portfolio limit breached — continue (soft)",
                    "{}");
                return true;
            }
            // Unwind (order-pipeline emergency liquidation, owned by
            // OrderIntentProcessor as of Phase 2) must run BEFORE trigger_halt:
            // unwind_positions bypasses process()'s halt gate but still needs
            // the router to accept the closing order while halt_flag_ is not
            // yet set. Preserves the exact pre-extraction order (see
            // engine-decomposition.md and risk_unwind_sink.h).
            if (config_.risk_unwind)
                risk_unwind_sink_.request_unwind(event_count);
            trigger_halt_("risk post-fill limit breached - engine halted");
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

void FillProcessor::notify_position_change_all(const std::string& symbol, bool open)
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
