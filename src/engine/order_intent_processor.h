#pragma once

// OrderIntentProcessor — domain-processor extraction that follows
// FillProcessor. See the "OrderIntentProcessor Preparation Report" for the
// original dependency/construction-order/cycle analysis, and the Phase 1/2/3
// implementation deliverables for what actually moved at each step.
//
// Phase 1 scope: the canonical normal order-submission path
// (process/unwind_positions/drain_async_submit_results/marked_account_equity).
//
// Phase 2 scope: routing/staging, stop triggering, resting-limit traversal,
// MM-crossing delivery, exit firing, strategy-exit registration/finalization,
// and pending-order due-order submission.
//
// Phase 3 scope (this addition): the remaining order-lifecycle boundary —
// cancel/modify, and the paired end-of-stream pending/day-order lifecycle
// ceremony. After this phase, engine no longer implements any order-domain
// behavior directly; it delegates entirely to orders_->... and its own
// cancel_order()/modify_order() are one-line public forwards (kept because
// external callers — tests today, potential operator/TUI callers later —
// still call engine.cancel_order(...)/engine.modify_order(...) directly).
//
// This is the terminal state for the order-domain boundary: canonical order
// attribution (OrderAttributionStore), pending scheduling
// (PendingOrderScheduler), pending stops, and pending cancel-acks are now
// ALL either a dedicated leaf collaborator or genuinely owned here — no
// order-domain state remains split across engine and this class purely for
// historical reasons. See the Phase 3 deliverable's state-ownership matrix.
//
// Depends only on named domain-subsystem references/pointers — no engine&,
// no EngineContext, no service locator (Check B in
// docs/architecture/05-engine-boundaries.md). Halt/log/publish reach engine
// exclusively through EngineHotPathSink& (engine_hotpath_sink.h). This class
// is itself referenced by nothing outside engine (no reverse dependency).
//
// LIVE-SAFETY SURFACE: this file is part of the frozen surface (see
// scripts/check-live-safety-freeze.sh). Any edit requires the
// LIVE_SAFETY_CCB_APPROVED token in the commit message.

#include "engine_config.h"
#include "engine_hotpath_sink.h"
#include "execution/portfolio.h"
#include "execution/order_tracker.h"
#include "execution/mark_point.h"
#include "risk/risk_accounting.h"
#include "risk/risk_manager.h"
#include "risk/futures_risk_check.h"
#include "analytics/analytics.h"
#include "order_audit_sink.h"
#include "execution_router.h"        // ExecutionRouter + pending_cancel_meta
#include "order_attribution_store.h"
#include "pending_order_scheduler.h"
#include "fill_processor.h"
#include "instrument_spec_cache.h"
#include "exits/exit_manager.h"
#include "market_maker/market_maker.h"
#include "orderbook/orderbook_registry.h"
#include "orderbook/orderbook.h"     // trades
#include "types/object_pool.h"
#include "types/pool_exhausted.h"
#include "core/event.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class DashboardSnapshotBuilder;   // pointer-only; may be null (matches FillProcessor's pattern)
class ShadowTracker;              // pointer-only; non-null only in shadow mode
class IExecutionAdapter;          // used only via shared_ptr/raw pointer
class IStrategy;                  // passed by the caller, never stored (see finalize_route)

class OrderIntentProcessor final
{
public:
    OrderIntentProcessor(
        portfolio& port,
        OrderTracker& order_tracker,
        RiskManager& risk_manager,
        IRiskCheck* risk_check,                        // nullable — provider may supply none
        Analytics& analytics,
        IOrderAuditSink& audit_sink,
        ExecutionRouter& router,
        FillProcessor& fills,
        OrderAttributionStore& attribution,
        PendingOrderScheduler& scheduler,
        truetest::exits::ExitManager& exit_manager,
        InstrumentSpecCache& instrument_spec_cache,
        MarketMaker& market_maker,
        OrderbookRegistry& orderbook_registry,
        std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>>& execution_adapters,
        ObjectPool<order_event>& order_pool,
        ObjectPool<rejection_event>& rejection_pool,
        ObjectPool<cancel_event>& cancel_pool,
        ObjectPool<amend_event>& amend_pool,
        const std::atomic<bool>& halt_flag,
        const std::atomic<bool>& pause_all,              // operator pause gate (route()/modify())
        std::atomic<double>& last_mid_price,            // read+write: anchor paths re-center it
        const std::string& last_mark_symbol,
        const std::unordered_map<std::string, mark_point>& last_mark_prices,
        std::mutex& last_mark_prices_mu,
        const std::chrono::system_clock::time_point& last_sim_time, // audit timestamps (cancel/modify/EOS)
        const std::chrono::system_clock::time_point& last_decision_ts, // F-08 decision clock
        const std::unordered_set<std::string>& l2_seeded_symbols,
        const bool& mm_threaded,                        // true while MarketMakerWorker owns the book
        DashboardSnapshotBuilder* dashboard_builder,     // nullable
        ShadowTracker* shadow_tracker,                   // nullable — non-null only in shadow mode
        portfolio* exchange_portfolio,                   // nullable — non-null only in shadow mode
        const engine_config& config,
        EngineHotPathSink& hotpath
    );

    // ---- Phase 1: canonical normal order-submission path -----------------
    bool process(const std::shared_ptr<order_event>& o,
                std::size_t& event_count, bool& halt_requested);
    void unwind_positions(std::size_t& event_count);
    void drain_async_submit_results(IExecutionAdapter* adapter);
    double marked_account_equity(std::string_view current_symbol,
                                 double current_mark) const;

    // ---- Phase 2: routing, triggering, exits ------------------------------

    // Verbatim move of the former engine::route_order. Assigns the order id,
    // registers attribution, applies the instrument-spec filter, then either
    // stages it (pending stop / latency queue / bar-delay queue) or submits
    // it immediately via process(). anchor_immediate: bracket/exit fires
    // execute now against a book re-centered at order.get_price() instead of
    // being deferred through execution_bar_delay.
    bool route(order_event& order,
              const std::chrono::system_clock::time_point& sim_time,
              std::size_t& event_count, bool& halt_requested,
              bool anchor_immediate = false);

    // Verbatim move of the former engine::check_pending_stops. Owns the
    // trigger-condition evaluation over pending_stops_ (owned here as of
    // Phase 3 — see the Preparation Report §8 for why the trigger logic
    // itself stayed out of PendingOrderScheduler regardless of who owns the
    // storage).
    void check_pending_stops(std::string_view event_symbol,
                             double open, double high, double low,
                             const std::chrono::system_clock::time_point& sim_time,
                             std::size_t& event_count, bool& halt_requested);

    // Verbatim move of the former engine::sweep_resting_limits.
    double sweep_resting_limits(const std::string& symbol,
                                double low, double high,
                                const std::chrono::system_clock::time_point& ts,
                                std::size_t& event_count, bool& halt_requested,
                                double bar_volume = 0.0);

    // Verbatim move of the former engine::deliver_mm_book_trades.
    void deliver_mm_book_trades(const std::string& symbol, const trades& trs,
                                const std::chrono::system_clock::time_point& ts,
                                std::size_t& event_count, bool& halt_requested);

    // Verbatim moves of the former engine::evaluate_exits overloads. Both
    // recurse into route(anchor_immediate=true) — the legitimate domain
    // cycle documented in the Phase 2 deliverable. ExitManager remains the
    // sole owner of exit policy/state; this only coordinates submission.
    bool evaluate_exits(const std::string& symbol, double px,
                        std::chrono::system_clock::time_point ts,
                        std::size_t& event_count,
                        std::int64_t recv_ns);
    bool evaluate_exits(const std::string& symbol,
                        double open, double low, double high, double close,
                        std::chrono::system_clock::time_point ts,
                        std::size_t& event_count,
                        std::int64_t recv_ns);

    // F-01(a): drains ExitManager's refused-bracket flatten requests and
    // routes each for the observed symbol at the captured triggering mark.
    // Returns true on a terminal halt, matching the
    // evaluate_exits contract. Called from both evaluate_exits overloads
    // before the armed brackets are probed.
    bool route_exit_flatten_requests(const std::string& symbol,
                                     std::size_t& event_count,
                                     std::int64_t recv_ns);

    // F-05a: latch (and report once) that the account's marked equity has
    // reached or crossed zero. Observation only — never liquidation.
    void note_marked_equity(double equity) const;

    // F-02: the single emission point for an order leaving the active
    // lifecycle without filling. Sets the terminal status, releases the
    // order's exit intent (or, for a closer, its reserved close quantity)
    // and resyncs the owning strategy's position gate. Every
    // set_status(..., rejected/cancelled/expired) in this class routes
    // through here — see the block comment on the definition.
    bool emit_terminal_transition(
        std::uint64_t order_id,
        const std::string& symbol,
        double qty,
        order_status terminal,
        std::chrono::system_clock::time_point authoritative_ts = {});




    // Verbatim move of the former engine::finalize_strategy_route (renamed
    // to drop the redundant "_route" stutter now that it hangs off orders_).
    // strategy is a caller-supplied parameter, never stored (the run loops
    // stay the sole owner of strategy_/additional_strategies_ dispatch).
    void finalize_route(IStrategy& strategy,
                        const std::string& strategy_name,
                        const order_event& order,
                        bool halted,
                        std::optional<double> pre_route_net_qty = std::nullopt);

    // Verbatim move of the former engine::drain_pending_orders's domain-glue
    // half (MM re-center + process() submission); the pure due/compaction
    // logic stays in PendingOrderScheduler, consumed here via its query/pop
    // API. Named drain_due to read cleanly as orders_->drain_due(...).
    void drain_due(const std::chrono::system_clock::time_point& sim_time,
                   std::size_t& event_count, bool& halt_requested,
                   std::string_view event_symbol);

    // ---- Phase 3: cancel/modify, end-of-stream lifecycle ------------------

    // Verbatim move of the former engine::cancel_order. Late-fill safety
    // note (unchanged from the original — this move does not touch the
    // semantics): a successful adapter cancel_order()/async cancel-pending
    // transition does NOT guarantee no fill can still arrive for order_id —
    // a venue-originated fill already in flight when the cancel request was
    // written is a documented residual risk, not something this method (or
    // its move into this class) claims to close. See engine::cancel_order's
    // original call sites and the trading-logic audit for the full caveat.
    bool cancel(const std::string& symbol, uint64_t order_id,
               const std::string& reason = "");

    // Verbatim move of the former engine::modify_order.
    bool modify(const std::string& symbol, uint64_t order_id,
               double new_price, double new_qty);

    // Consolidates the former engine::drain_final_pending (EOS expiry of
    // never-submitted latency/bar-delay orders) + engine::cancel_day_orders
    // (EOS cancellation of resting DAY-TIF orders). Both bodies moved
    // verbatim; merged into one public entry point because every one of
    // their 5 call sites already invoked them back-to-back with no
    // intervening logic — two historical engine methods for one semantic
    // "end of stream" order-lifecycle operation (avoids the wrapper-per-
    // historical-method pattern the Phase 3 task explicitly asked to avoid).
    void finalize_end_of_stream(std::size_t& event_count, bool& halt_requested);

    // Narrow lifecycle-reset hooks for state now owned here (Phase 3), used
    // only by engine's own lifecycle methods — never called from order-
    // domain code itself. Kept separate (not merged into one "reset()")
    // because their callers fire at different times: clear_pending_stops()
    // from engine::clear_pending_state() (every run() start),
    // clear_pending_cancels() from engine::reset_for_next_trial() (MC trial
    // boundary only) — merging them would change when each actually clears.
    void clear_pending_stops() noexcept { pending_stops_.clear(); }
    void clear_pending_cancels() noexcept { pending_cancels_.clear(); }

private:
    // Former engine::register_strategy_exit_intent — private now: its only
    // caller was finalize_strategy_route/finalize_route, both before and
    // after this move.
    void register_strategy_exit_intent(IStrategy& strategy,
                                       const std::string& strategy_name,
                                       const order_event& order,
                                       std::optional<double> pre_route_net_qty = std::nullopt);

    // Former engine::resolve_instrument_spec / apply_instrument_spec —
    // private now: route()'s own instrument-spec-filter helpers, no other
    // caller.
    const instrument_spec* resolve_instrument_spec(const std::string& symbol);
    bool apply_instrument_spec(order_event& o, const instrument_spec& spec) const;

    // R3: fill the authoritative mark-to-market views on `snap` for this
    // candidate order (position ledger + open-order ledger + timestamped
    // marks). Holds last_mark_prices_mu_ for exactly one pass.
    void build_authoritative_risk_view(const order_event& order,
                                       risk_snapshot& snap) const;

    // Former engine::mid_for_symbol — private now: drain_due()'s own
    // per-order mark lookup, no other caller (mirrors marked_account_equity,
    // which stayed public because a second, still-engine-owned caller needs
    // it; this one has exactly one caller after the move).
    double mid_for_symbol(const std::string& symbol) const;

    // Acquire-with-halt-on-exhaustion, mirrors engine::acquire_pooled<T>
    // exactly, scoped to the four pools this class actually acquires from.
    template<typename T, std::size_t BlockSize, typename... Args>
    std::shared_ptr<T> acquire_pooled(ObjectPool<T, BlockSize>& pool, Args&&... args)
    {
        try
        {
            return pool.acquire(std::forward<Args>(args)...);
        }
        catch (const pool_exhausted& e)
        {
            hotpath_.trigger_halt(e.what());
            throw;
        }
    }

    // References to canonical state owned elsewhere — never duplicated.
    portfolio& portfolio_;
    OrderTracker& order_tracker_;
    RiskManager& risk_manager_;
    IRiskCheck* risk_check_;
    Analytics& analytics_;
    IOrderAuditSink& audit_sink_;
    ExecutionRouter& router_;
    FillProcessor& fills_;
    OrderAttributionStore& attribution_;
    PendingOrderScheduler& pending_scheduler_;
    truetest::exits::ExitManager& exit_manager_;
    InstrumentSpecCache& instrument_spec_cache_;
    MarketMaker& market_maker_;
    OrderbookRegistry& orderbook_registry_;
    std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>>& execution_adapters_;
    ObjectPool<order_event>& order_pool_;
    ObjectPool<rejection_event>& rejection_pool_;
    ObjectPool<cancel_event>& cancel_pool_;
    ObjectPool<amend_event>& amend_pool_;
    const std::atomic<bool>& halt_flag_;
    const std::atomic<bool>& pause_all_;
    std::atomic<double>& last_mid_price_;
    const std::string& last_mark_symbol_;
    const std::unordered_map<std::string, mark_point>& last_mark_prices_;
    std::mutex& last_mark_prices_mu_;
    const std::chrono::system_clock::time_point& last_sim_time_;
    // F-08: when the information behind the current observation existed.
    // Stamped onto every routed order so time-windowed risk rules and the
    // audit trail stop inheriting the bar-open clock. See order_event.
    const std::chrono::system_clock::time_point& last_decision_ts_;

    const std::unordered_set<std::string>& l2_seeded_symbols_;
    const bool& mm_threaded_;
    DashboardSnapshotBuilder* dashboard_builder_;
    ShadowTracker* shadow_tracker_;
    portfolio* exchange_portfolio_;
    const engine_config& config_;
    EngineHotPathSink& hotpath_;

    // Owned outright as of Phase 3 (canonical single owner — see the Phase 3
    // deliverable's state-ownership matrix): both readers and both writers
    // of each are exclusively methods on this class. No external reference
    // exists (ExecutionRouter's former pending_cancels_ reference was
    // confirmed dead code and removed rather than repointed).
    std::vector<std::shared_ptr<order_event>> pending_stops_;
    std::unordered_map<uint64_t, pending_cancel_meta> pending_cancels_;
};
