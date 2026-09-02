#pragma once
// FillProcessor — Phase 2 engine decomposition (domain-processor extraction, 2026-08).
// See core/docs/internal/engine-decomposition.md "Phase 2: Domain Processors" for the
// before/after call-order evidence, dependency rationale, and state-ownership table this
// class was extracted under.
//
// Owns the canonical fill pipeline: given one venue/paper fill, it updates order status,
// canonical portfolio/lot state, strategy notifications, adverse-selection/exit/risk
// bookkeeping, the audit trail, and dashboard caches, in the exact sequence previously
// documented inline in engine.cpp's "CANONICAL HOT-PATH ORDERING" comment. It coordinates
// existing subsystems (Portfolio, OrderTracker, ExitManager, RiskManager, ExecutionRouter,
// IOrderAuditSink, Analytics) — it does not duplicate any of them, and it does not own any
// canonical trading state beyond the one soft-post-fill-breach counter it exclusively
// writes (see state-ownership table).
//
// OrderIntentProcessor preparatory extraction (see the "OrderIntentProcessor
// Preparation Report" §7): order attribution metadata (opener_order_id /
// strategy_name per order_id) is read through attribution_
// (OrderAttributionStore&) rather than a raw engine-owned map. This removes
// the construction-order cycle an OrderIntentProcessor -> this class ->
// OrderIntentProcessor-owned attribution design would otherwise create:
// FillProcessor is constructed before OrderIntentProcessor (now fully
// extracted — see order_intent_processor.h) and therefore needs a stable,
// independently-constructed attribution owner. Nothing else about this
// class changed — same subsystems, same call order. FillProcessor has no
// dependency on OrderIntentProcessor itself (see the layer-deps Check B
// guard and the OrderIntentProcessor closure report for the verified
// absence of any such back-reference).
//
// LIVE-SAFETY SURFACE: this file is part of the frozen surface (see
// scripts/check-live-safety-freeze.sh). Any edit requires the LIVE_SAFETY_CCB_APPROVED
// token in the commit message.

#include "engine_config.h"
#include "execution/portfolio.h"
#include "execution/order_tracker.h"
#include "exits/exit_manager.h"
#include "risk/risk_manager.h"
#include "analytics/adverse_selection_tracker.h"
#include "analytics/analytics.h"
#include "order_audit_sink.h"
#include "execution_router.h"         // ExecutionRouter
#include "order_attribution_store.h"  // OrderAttributionStore
#include "engine_hotpath_sink.h"       // IEngineHotPathSink
#include "risk_unwind_sink.h"         // IRiskUnwindSink
#include "types/object_pool.h"
#include "strategy/strategy_interface.h"
#include "core/event.h"

#include <memory>
#include <string>
#include <vector>

#ifdef HAS_DEBUG
#include "debug/stage_timer.h"
#endif

class DashboardSnapshotBuilder;   // pointer-only; may be null pre-construction guard parity
class ShadowTracker;              // pointer-only; non-null only in shadow mode
class IExecutionAdapter;          // used only via shared_ptr in process_adapter_fills

class FillProcessor final
{
public:
    FillProcessor(
        portfolio& portfolio,
        OrderTracker& order_tracker,
        truetest::exits::ExitManager& exit_manager,
        RiskManager& risk_manager,
        AdverseSelectionTracker& adverse_selection,
        Analytics& analytics,
        const std::unique_ptr<IOrderAuditSink>& audit_sink,
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
        IEngineHotPathSink& hotpath,
        IRiskUnwindSink& risk_unwind_sink
#ifdef HAS_DEBUG
        , debug::StageTimer& stage_timer
#endif
    );

    // Canonical engine-book fill pipeline (tracker, portfolio, strategy, exits, risk,
    // audit, analytics, publish). Returns false if post-fill risk triggered a terminal
    // halt. run_post_fill_risk: false for risk_unwind fills (already halting).
    // mark_shadow_sim: true for paper/sim fills in shadow dual-track mode. status_reason:
    // optional audit reason (e.g. "risk_unwind"). Verbatim move of the former
    // engine::handle_engine_fill — see engine-decomposition.md for the call-site mapping.
    bool handle_fill(fill_event& f,
                     std::size_t& event_count,
                     bool& halt_requested,
                     bool run_post_fill_risk = true,
                     bool mark_shadow_sim = false,
                     const char* status_reason = nullptr);

    // Canonical fill pipeline for one adapter's pending fills (poll + apply). Returns
    // false on a post-fill risk halt.
    bool process_adapter_fills(const std::shared_ptr<IExecutionAdapter>& adapter,
                               std::size_t& event_count, bool& halt_requested);

    // Stamp per-lot attribution (opener_order_id + strategy_name) onto a fill_event if
    // not already present. Public: also called directly from engine's shadow-mode fill
    // branches, not just internally by handle_fill.
    void stamp_fill_attribution(fill_event& f) const;

    // Legacy net-truth push for strategies that still override set_position_open, plus
    // net-flat bracket sweep. Public: also called from engine's finalize_strategy_route
    // (order pipeline) on pause/reject resync, not just from handle_fill.
    void notify_position_change_all(const std::string& symbol, bool open);

    // State ownership: this counter's only writer is handle_fill (soft post-fill risk
    // breach path, backtest-only). Canonical owner moved here from engine::
    // soft_post_fill_breaches_; engine reads/resets via these accessors.
    std::size_t soft_post_fill_breach_count() const noexcept { return soft_post_fill_breaches_; }
    void reset_soft_post_fill_breaches() noexcept { soft_post_fill_breaches_ = 0; }

private:
    void dispatch_fill_to_strategy(const fill_event& f) const;

    // Thin, non-domain attribution lookups delegating to attribution_ (not a
    // duplicated subsystem — OrderAttributionStore is the sole canonical
    // owner/writer of order attribution metadata; see order_attribution_store.h).
    uint64_t lookup_opener(uint64_t order_id) const;
    const std::string& lookup_strategy_name(uint64_t order_id) const;

    // Acquire-with-halt-on-exhaustion, scoped to fill_pool_ only (mirrors
    // engine::acquire_pooled<T>, narrowed to the one pool this class needs).
    std::shared_ptr<fill_event> acquire_pooled_fill(const fill_event& f);

    // References to canonical state owned elsewhere — never duplicated.
    portfolio& portfolio_;
    OrderTracker& order_tracker_;
    truetest::exits::ExitManager& exit_manager_;
    RiskManager& risk_manager_;
    AdverseSelectionTracker& adverse_selection_;
    Analytics& analytics_;
    const std::unique_ptr<IOrderAuditSink>& audit_sink_;
    ExecutionRouter& router_;
    ObjectPool<fill_event>& fill_pool_;
    const OrderAttributionStore& attribution_;
    std::shared_ptr<IStrategy>& strategy_;
    std::vector<std::shared_ptr<IStrategy>>& additional_strategies_;
    std::vector<std::string>& additional_strategy_names_;
    const std::string& primary_strategy_name_;
    const engine_config& config_;
    DashboardSnapshotBuilder* dashboard_builder_;
    ShadowTracker* shadow_tracker_;

    // Shared narrow contract onto engine's event-log writer, ring-dispatch
    // policy, and terminal halt entry point. This mirrors OrderIntentProcessor
    // instead of maintaining three separate type-erased callbacks.
    IEngineHotPathSink& hotpath_;

    // Emergency-liquidation request (Phase 2 OrderIntentProcessor extraction):
    // narrow interface reference, not a std::function — see risk_unwind_sink.h
    // for the construction-order proof this is the required shape (engine
    // implements it, forwarding to orders_->unwind_positions(...) only at
    // invocation time, since OrderIntentProcessor is constructed after
    // FillProcessor).
    IRiskUnwindSink& risk_unwind_sink_;

#ifdef HAS_DEBUG
    debug::StageTimer& stage_timer_;
#endif

    std::size_t soft_post_fill_breaches_ = 0;
};
