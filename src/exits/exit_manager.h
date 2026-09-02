#pragma once

#include "core/event.h"
#include "exits/exit_intent.h"
#include "exits/bracket_adapter.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace truetest::exits {

// Engine-side enforcement of strategy-declared exit intents. Keyed by
// opener_order_id so two concurrent entries on the same (strategy,symbol)
// carry independent stops. Lifecycle: register_pending at entry submit ->
// on_fill promotes to armed (using actual fill px/qty) -> on_price returns
// every armed intent that triggers on this tick.
class ExitManager
{
public:
    // Deliberately trivial: an eager reserve here measurably moved the
    // armed_ nodes in the heap and cost ~8% on on_bar. armed_this_window_
    // holds at most one entry per opener armed inside a single observation,
    // so it grows to a handful of elements and stays there.
    ExitManager() = default;



    // Adds a pending intent for intent.opener_order_id. Calling again with
    // the same opener_id appends a second intent (TP1/TP2/SL scale-out).
    void register_pending(exit_intent intent);

    // Promotes all pending intents keyed by f.get_order_id() into armed,
    // using the fill price as entry reference and fill qty as the size.
    void on_fill(const fill_event& f);

    // Opener- or closer-aware variant. Passing the opener_order_id lets
    // the manager distinguish the two roles: when opener==fill.order_id
    // (the fill is the opener itself), pending intents are promoted to
    // armed; when opener != fill.order_id (this fill closes an earlier
    // entry), the armed bracket for that opener is dropped so it can't
    // fire again on a lot that's already been closed by signal.
    void on_fill(const fill_event& f, std::uint64_t opener_order_id);

    // Account a quantity component of one physical fill against an opener.
    // This changes bracket state only: the fill itself remains one canonical
    // ledger/audit/strategy/replay event. Used for a crossing fill's close and
    // residual-open components.
    void on_fill(const fill_event& f, std::uint64_t opener_order_id,
                 double accounted_qty);

    // F-01(b) — opens one evaluation window per market observation. The
    // engine calls this before it drains delayed orders, so every intent
    // armed by a fill *inside* this observation carries the current epoch.
    // on_bar then knows that this bar's OPEN is a price printed before the
    // bracket existed, and refuses to anchor a gap fill there.
    //
    // This defers the *fill price*, never the protection: a bar that wicks
    // through the stop after the entry filled at its open still stops out,
    // at the level rather than at the open.
    //
    // Deliberately not a timestamp comparison. A bar's timestamp is its OPEN
    // time (F-08) and fill_event carries no independent clock, so no clock
    // available here can order a fill against the bar that produced it.
    //
    // Fail-safe: if a call site forgets to open a window, on_bar/on_price
    // still advance the epoch themselves. The degraded behaviour is the
    // pre-fix anchoring, never a silent suppression of a stop.
    void begin_evaluation_window();


    // Returns every armed intent at this symbol whose SL/TP/trailing/time
    // trigger crossed. Returned order_events carry opener_order_id so the
    // portfolio can close the correct lot.
    std::vector<order_event> on_price(const std::string& symbol,
                                      double px,
                                      std::chrono::system_clock::time_point ts);

    // Bar-aware variant: probes each armed intent against the bar's worst-
    // and best-case extremes (longs: low for SL, high for TP/trail; shorts:
    // inverted) so an intra-bar wick through the bracket fires it instead
    // of being missed when only the close is checked. Conservative ordering:
    // SL is evaluated before TP if both extremes crossed in the same bar,
    // and the trailing stop is tested at its pre-bar level (this bar's
    // favorable extreme raises the trail only for subsequent bars —
    // assuming the favorable extreme printed first would be look-ahead).
    // The returned order's price is the anchored fire price: the SL/TP
    // level itself, or the bar open when the bar gapped through it.
    std::vector<order_event> on_bar(const std::string& symbol,
                                    double open, double low, double high,
                                    double close,
                                    std::chrono::system_clock::time_point ts);

    // Drop everything for one opener (used when strategy exits via its own
    // signal path and the armed bracket should no longer fire).
    void cancel(std::uint64_t opener_order_id);

    // F-02 legacy release for a non-protective attributed close. Engine-owned
    // protective closes use the ticketed terminal path below; collapsing the
    // two would make a rejected stop look like an ordinary scale-out.
    void release_close_reservation(std::uint64_t opener_order_id, double qty);

    // A fired bracket or slippage-disarm flatten gets a ticket before route()
    // assigns an order id.  Binding happens immediately after id allocation,
    // before an instrument/risk check or synchronous adapter fill can take a
    // terminal path.  A terminal protective failure is deliberately not
    // retried: the caller must enter its write-once emergency halt path.
    // Non-owning view into fired_protections_. Valid only until this manager
    // mutates or erases the referenced ticket. In particular, callers of the
    // terminal callback below must consume the view inside that callback.
    struct protective_exit_view
    {
        std::uint64_t ticket = 0;
        std::uint64_t order_id = 0;
        std::uint64_t opener_order_id = 0;
        double requested_qty = 0.0;
        double filled_qty = 0.0;
        double remaining_qty = 0.0;
        order_exit_reason reason = order_exit_reason::none;
        std::string_view symbol;
        std::string_view strategy_name;
    };
    bool bind_protective_exit(std::uint64_t ticket, std::uint64_t order_id);
    std::optional<protective_exit_view> protective_exit_for_order(
        std::uint64_t order_id) const;
    template <typename Observer>
    bool on_protective_close_terminal(std::uint64_t order_id,
                                      Observer&& observe_before_erase)
    {
        const auto protective = protective_exit_for_order(order_id);
        if (!protective) return false;

        // Restore only the unfilled reservation for diagnostics/lot
        // accounting. Never re-arm or retry a stop that already fired.
        if (protective->remaining_qty > 0.0)
            opener_remaining_qty_[protective->opener_order_id] +=
                protective->remaining_qty;
        completed_protective_close_orders_.insert(order_id);

        // The strings in the view belong to fired_protections_. Consume them
        // before erasing that record. Cleanup also happens when an observer
        // throws, preserving the old terminal/no-retry state transition.
        try
        {
            std::forward<Observer>(observe_before_erase)(*protective);
        }
        catch (...)
        {
            protective_ticket_by_order_.erase(order_id);
            fired_protections_.erase(protective->ticket);
            throw;
        }
        protective_ticket_by_order_.erase(order_id);
        fired_protections_.erase(protective->ticket);
        return true;
    }
    bool on_protective_close_terminal(std::uint64_t order_id)
    {
        return on_protective_close_terminal(
            order_id, [](const protective_exit_view&) noexcept {});
    }
    bool protective_fill_is_admissible(std::uint64_t order_id,
                                       double qty) const;
    bool is_known_protective_close(std::uint64_t order_id) const;
    double reserved_close_qty(const std::string& symbol,
                              order_side side) const;


    // Legacy bulk-cancel: remove every pending/armed intent for a given
    // (strategy,symbol). Preserved for the net-flat notifier path; becomes
    // unused once strategies own their entry gating.
    void cancel(const std::string& strategy_name, const std::string& symbol);

    std::size_t armed_count() const { return armed_.size(); }
    std::size_t pending_count() const { return pending_.size(); }

    // F-01(a) — an opener whose entry slippage reached or exceeded its own
    // designed stop distance. The bracket is NOT armed: past that point the
    // trade's risk premise is void, and shifting the stop by the slippage
    // would place it on the wrong side of the market, producing a phantom
    // stop-out at a price the market never traded at. The engine drains
    // these and flattens the lot instead. price is left for the caller to
    // anchor at the current mark — ExitManager has no mark of its own.
    struct flatten_request
    {
        std::string   symbol;
        std::string   strategy_name;
        order_side    close_side = order_side::sell;
        double        qty = 0.0;
        std::uint64_t opener_order_id = 0;
        double        entry_price = 0.0;
        double        designed_stop_distance = 0.0;
        double        entry_slippage = 0.0;
        double        trigger_price = 0.0;
        std::chrono::system_clock::time_point trigger_ts{};
        std::uint64_t protective_exit_ticket = 0;
    };
    bool has_flatten_requests() const { return !flatten_requests_.empty(); }
    std::vector<flatten_request> take_flatten_requests();
    std::vector<flatten_request> take_flatten_requests_for(std::string_view symbol);

    // F-06 — observable intent lifecycle. Without these a leaked pending
    // intent is invisible until the container itself is inspected.
    struct lifecycle_counters
    {
        std::size_t pending_registered   = 0;
        std::size_t armed                = 0;
        std::size_t deferred_arms        = 0;  // F-03: armed from an orphan fill
        std::size_t cancelled            = 0;
        std::size_t pending_evicted      = 0;  // F-06: capacity bound hit
        std::size_t orphan_fills_recorded = 0; // F-03
        std::size_t orphan_fills_evicted  = 0;
        std::size_t slippage_disarms     = 0;  // F-01(a)
        std::size_t flatten_requests     = 0;  // F-01(a)
    };
    const lifecycle_counters& counters() const noexcept { return counters_; }

    // Live opener count for a (strategy,symbol) pair across pending+armed.
    // Used by the engine to distinguish single-lot strategies (where a
    // net-flat transition can safely bulk-cancel any leftover bracket)
    // from multi-lot strategies (which own opener_order_id discipline).
    std::size_t openers_for(const std::string& strategy_name,
                            const std::string& symbol) const;

    // Optional venue-bracket integration. nullptr -> engine-side eval is
    // the only enforcement (current backtest/shadow behavior). Setting
    // an adapter activates defense-in-depth: the in-process armed intent
    // stays armed AND the venue gets resting orders. Set once at startup.
    void set_bracket_adapter(std::shared_ptr<IBracketAdapter> adapter)
    {
        bracket_adapter_ = std::move(adapter);
    }
    bool has_bracket_adapter() const
    {
        return static_cast<bool>(bracket_adapter_);
    }

    // Reverse-lookup for fills coming back from the venue. The engine
    // calls this on every inbound exec report - non-zero means the fill
    // is a venue-bracket leg and the engine should stamp opener_order_id
    // before routing through the closer-fill path. Thread-safe - may be
    // called from a provider's WS thread (e.g. user-data stream).
    std::uint64_t opener_for_exchange_order(const std::string& exchange_order_id) const;

    // Atomically snapshots all attribution the bridge needs for one
    // venue-managed bracket fill. The user-data worker must not observe a
    // partially removed reverse map while the engine thread cancels a
    // bracket.
    struct venue_fill_attribution
    {
        std::uint64_t opener_order_id = 0;
        std::string strategy_name;
        double intended_price = 0.0;
    };
    std::optional<venue_fill_attribution> venue_fill_attribution_for_exchange_order(
        const std::string& exchange_order_id) const;

    // Companion lookup so the engine can populate order_meta_ with the
    // right strategy_name when it synthesizes an order_id for a venue
    // bracket leg. Empty string = no match. Thread-safe.
    std::string strategy_name_for_exchange_order(const std::string& exchange_order_id) const;

    // Planned trigger/limit price for a venue-managed bracket leg. Zero means
    // that the venue did not expose a price for that leg (for example, an
    // opaque recovered order); callers must retain that absence rather than
    // substituting the execution price as an intended price.
    double intended_price_for_exchange_order(const std::string& exchange_order_id) const;

    // Restart recovery: install an armed intent + venue handles for a
    // bracket the venue still has resting from a previous engine run.
    // Engine calls this at startup after the adapter enumerates open
    // brackets via IBracketAdapter::list_open(). The in-process armed
    // intent is what evaluate_exits checks against - the venue handles
    // are what cancel() forwards to. Both come back online together.
    void rehydrate(const IBracketAdapter::recovered_bracket& rb);

    // Phase A (MC object reuse): clears all pending/armed brackets and
    // venue state so the manager can be reused for the next trial.
    void reset();

    // Read-only snapshot of armed brackets for the live TUI. Designed
    // to be called from the engine's snapshot path (main thread). Each
    // row carries the in-process intent + whether the venue has the
    // bracket as a resting order (handles_ entry exists) so the panel
    // can show "engine-only" vs "venue-resting" state.
    struct armed_view
    {
        std::uint64_t opener_order_id = 0;
        std::string   strategy_name;
        std::string   symbol;
        order_side    close_side = order_side::sell;
        double        qty = 0.0;
        double        entry_price = 0.0;
        std::optional<double> stop_loss;
        std::optional<double> take_profit;
        bool          venue_managed = false;   // handles_ entry exists
        std::string   venue_list_id;           // empty if not OCO/grouped
        std::chrono::system_clock::time_point ts_armed{};
    };
    std::vector<armed_view> snapshot_armed() const;

private:
    struct armed_intent
    {
        exit_intent intent;
        double      entry_price = 0.0;
        double      best_price  = 0.0;  // running MFE for trailing
        std::chrono::system_clock::time_point ts_armed{};
        // NOTE: this struct is walked in full on every market observation, so
        // its size is a per-event cost — eight extra bytes here measured as a
        // ~14% regression on on_bar with 64 armed brackets, purely from
        // crossing a cache line. Neither F-01(b)'s arming epoch nor F-09a's
        // designed risk distance is stored here for that reason: the epoch
        // lives in armed_this_window_ (consulted only when a bracket fires)
        // and the distance is recomputed from the jointly-shifted levels.

        // F-09a: the risk distance this bracket was designed with, recovered

        // from the (jointly shifted) levels rather than stored.
        double designed_stop_distance() const
        {
            if (!intent.reference_entry || !intent.stop_loss) return 0.0;
            const double d = *intent.reference_entry - *intent.stop_loss;
            return d < 0.0 ? -d : d;
        }
    };

    struct fired_protection
    {
        std::uint64_t ticket = 0;
        std::uint64_t order_id = 0;
        std::uint64_t opener_order_id = 0;
        double requested_qty = 0.0;
        double remaining_qty = 0.0;
        order_exit_reason reason = order_exit_reason::none;
        order_side close_side = order_side::sell;
        std::string symbol;
        std::string strategy_name;
    };


    // F-03: an opener fill that arrived before its exit intent was
    // registered. With execution_bar_delay == 0 the fill happens inside
    // route(), while register_pending runs afterwards in finalize_route —
    // so the promotion pending_ -> armed_ that on_fill would have done has
    // already been missed by the time the intent exists. Recording the fill
    // here makes arming order-independent instead of dependent on the
    // engine calling in one particular sequence.
    struct orphan_opener_fill
    {
        double        qty = 0.0;
        double        vwap = 0.0;
        std::chrono::system_clock::time_point ts{};
        std::uint64_t epoch = 0;          // window it was observed in
        bool          remaining_applied = false;
    };


    using strategy_symbol_key = std::pair<std::string, std::string>;
    struct ss_hash
    {
        std::size_t operator()(const strategy_symbol_key& k) const noexcept
        {
            std::hash<std::string> h;
            return h(k.first) ^ (h(k.second) << 1);
        }
    };

    // Keyed by opener_order_id. Multimap so a single entry can carry
    // TP1/TP2/SL scale-outs.
    std::unordered_multimap<std::uint64_t, exit_intent>  pending_;
    std::unordered_multimap<std::uint64_t, armed_intent> armed_;
    std::unordered_map<std::uint64_t, double> opener_remaining_qty_;
    std::unordered_map<std::uint64_t, double> opener_close_in_flight_qty_;
    std::unordered_map<std::uint64_t, fired_protection> fired_protections_;
    std::unordered_map<std::uint64_t, std::uint64_t> protective_ticket_by_order_;
    std::unordered_set<std::uint64_t> completed_protective_close_orders_;
    std::uint64_t next_protective_ticket_ = 1;

    std::unordered_map<std::uint64_t, orphan_opener_fill> orphan_opener_fills_;
    std::vector<flatten_request> flatten_requests_;
    lifecycle_counters counters_{};

    // F-01(b): openers armed during the CURRENT evaluation window. Kept out
    // of armed_intent deliberately (see the note there): it is read only when
    // a bracket actually fires, which is rare, and is cleared at every window
    // boundary so it can never leak. Almost always empty, so the fire-path
    // check is one compare against size().
    std::vector<std::uint64_t> armed_this_window_;
    bool armed_in_this_window(std::uint64_t opener) const noexcept
    {
        for (auto id : armed_this_window_)
            if (id == opener) return true;
        return false;
    }
    // Pathological guard for an embedder that arms without ever evaluating.
    // Dropping the flags degrades to the pre-fix anchoring, never to a
    // suppressed stop.
    static constexpr std::size_t kMaxArmedPerWindow = 1024;

    std::uint64_t eval_epoch_ = 0;

    // Set once the engine drives windows explicitly. Embedders that never
    // call begin_evaluation_window (unit tests, tools) keep the pre-fix
    // self-advancing behaviour instead of freezing every armed bracket.
    bool engine_drives_windows_ = false;


    // F-06 / F-03: hard bounds. pending_ is erased only by an opener fill or
    // an explicit cancel(); an order that dies any other way (rejected after
    // registration — see F-02) leaves its intent behind forever. The bound
    // converts an unbounded leak into a loud, counted eviction. Sized well
    // above any plausible live open-intent count so a healthy run never
    // evicts.
    static constexpr std::size_t kMaxPendingIntents    = 4096;
    static constexpr std::size_t kMaxOrphanOpenerFills = 1024;

    // F-01(a): openers whose bracket was refused because entry slippage
    // reached the designed stop distance. Two jobs: warn at most once per
    // opener (a pathological run must not turn the hot path into a log
    // flood), and route any *later* partial fill of the same opener into an
    // additional flatten request instead of silently leaving that residual
    // exposure unprotected. Erased when the opener is cancelled — which the
    // flatten fill itself triggers through the closer path.
    std::unordered_set<std::uint64_t> disarmed_openers_;



    // Reverse index supporting legacy cancel(strategy,symbol).
    std::unordered_multimap<strategy_symbol_key, std::uint64_t, ss_hash>
        strategy_symbol_to_openers_;

    // Venue-bracket state. handles_ keyed by opener so cancel paths can
    // forward to the adapter; exchange_to_leg_ is the reverse for
    // inbound venue fills that need to be stamped as closers. Mutated on
    // the engine thread; read on the provider's WS thread - guarded by
    // venue_mu_ which only wraps these three fields. None of the other
    // ExitManager containers are touched off-thread.
    struct exchange_leg
    {
        std::uint64_t opener_order_id = 0;
        std::string   strategy_name;
        double        intended_price = 0.0;
    };
    std::shared_ptr<IBracketAdapter> bracket_adapter_;
    std::unordered_map<std::uint64_t, bracket_handles> handles_;
    std::unordered_map<std::string, exchange_leg>      exchange_to_leg_;
    mutable std::mutex venue_mu_;

    void untrack_opener(std::uint64_t opener_order_id,
                        const std::string& strategy_name,
                        const std::string& symbol);

    double consume_opener_qty(std::uint64_t opener_order_id, double requested_qty);
    std::uint64_t remember_fired_protection(std::uint64_t opener_order_id,
                                            double qty,
                                            order_exit_reason reason,
                                            order_side close_side,
                                            std::string_view symbol,
                                            std::string_view strategy_name);

    // F-06: drop the oldest pending intents once the bound is reached, and
    // report it. Loud by design — a silent drop here is an unprotected lot.
    void enforce_pending_bound();

    // F-03: expire orphan opener fills older than one evaluation window and
    // enforce the capacity bound.
    void sweep_orphan_opener_fills();

    // F-01(a)/F-09a: shift SL/TP by the entry-relative delta, returning false
    // when the shift would place the stop through the market. Shared by the
    // arming path and the multi-level opener re-anchor.
    static bool shift_entry_relative_levels(exit_intent& intent,
                                            double designed_stop_distance,
                                            double new_entry);

    // F-01(a): refuse to arm this opener and ask the engine to flatten it.
    void request_flatten(std::uint64_t opener_order_id,
                         const exit_intent& intent,
                         double qty, double entry_price,
                         double designed_stop_distance,
                         double entry_slippage,
                         std::chrono::system_clock::time_point trigger_ts);

    // Drops every armed intent for the opener and files the flatten request.
    void disarm_and_flatten(std::uint64_t opener_order_id,
                            const exit_intent& sample,
                            double qty, double entry_price,
                            double designed_stop_distance,
                            double entry_slippage,
                            std::chrono::system_clock::time_point trigger_ts);

    // F-03: record an opener fill that arrived before its intent existed.
    void record_orphan_opener_fill(std::uint64_t opener_order_id,
                                   const fill_event& f, double qty);

    // Arms one intent from a known opener fill. Returns false when the
    // entry slippage voids the trade's risk premise (F-01(a)); the caller
    // then disarms the whole opener rather than arming a sibling on a
    // premise that no longer holds.
    bool arm_one(std::uint64_t opener_order_id,
                 exit_intent intent,
                 double fill_price, double fill_qty,
                 std::chrono::system_clock::time_point ts,
                 bool deferred);



    // Single point that drops handles_ + exchange_to_opener_ entries and
    // tells the adapter to clean up venue-side. Called from every cancel
    // path so adapter.cancel() is invoked exactly once per opener.
    void release_venue_bracket(std::uint64_t opener_order_id);
};

}
