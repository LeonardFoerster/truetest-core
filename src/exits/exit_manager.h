#pragma once

#include "core/event.h"
#include "exits/exit_intent.h"
#include "exits/bracket_adapter.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace truetest::exits {

// Engine-side enforcement of strategy-declared exit intents. Keyed by
// opener_order_id so two concurrent entries on the same (strategy,symbol)
// carry independent stops. Lifecycle: register_pending at entry submit →
// on_fill promotes to armed (using actual fill px/qty) → on_price returns
// every armed intent that triggers on this tick.
class ExitManager
{
public:
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
    // SL is evaluated before TP if both extremes crossed in the same bar.
    std::vector<order_event> on_bar(const std::string& symbol,
                                    double low, double high, double close,
                                    std::chrono::system_clock::time_point ts);

    // Drop everything for one opener (used when strategy exits via its own
    // signal path and the armed bracket should no longer fire).
    void cancel(std::uint64_t opener_order_id);

    // Legacy bulk-cancel: remove every pending/armed intent for a given
    // (strategy,symbol). Preserved for the net-flat notifier path; becomes
    // unused once strategies own their entry gating.
    void cancel(const std::string& strategy_name, const std::string& symbol);

    std::size_t armed_count() const { return armed_.size(); }
    std::size_t pending_count() const { return pending_.size(); }

    // Live opener count for a (strategy,symbol) pair across pending+armed.
    // Used by the engine to distinguish single-lot strategies (where a
    // net-flat transition can safely bulk-cancel any leftover bracket)
    // from multi-lot strategies (which own opener_order_id discipline).
    std::size_t openers_for(const std::string& strategy_name,
                            const std::string& symbol) const;

    // Optional venue-bracket integration. nullptr → engine-side eval is
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
    // calls this on every inbound exec report — non-zero means the fill
    // is a venue-bracket leg and the engine should stamp opener_order_id
    // before routing through the closer-fill path. Thread-safe — may be
    // called from a provider's WS thread (e.g. user-data stream).
    std::uint64_t opener_for_exchange_order(const std::string& exchange_order_id) const;

    // Companion lookup so the engine can populate order_meta_ with the
    // right strategy_name when it synthesizes an order_id for a venue
    // bracket leg. Empty string = no match. Thread-safe.
    std::string strategy_name_for_exchange_order(const std::string& exchange_order_id) const;

    // Restart recovery: install an armed intent + venue handles for a
    // bracket the venue still has resting from a previous engine run.
    // Engine calls this at startup after the adapter enumerates open
    // brackets via IBracketAdapter::list_open(). The in-process armed
    // intent is what evaluate_exits checks against — the venue handles
    // are what cancel() forwards to. Both come back online together.
    void rehydrate(const IBracketAdapter::recovered_bracket& rb);

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

    // Reverse index supporting legacy cancel(strategy,symbol).
    std::unordered_multimap<strategy_symbol_key, std::uint64_t, ss_hash>
        strategy_symbol_to_openers_;

    // Venue-bracket state. handles_ keyed by opener so cancel paths can
    // forward to the adapter; exchange_to_leg_ is the reverse for
    // inbound venue fills that need to be stamped as closers. Mutated on
    // the engine thread; read on the provider's WS thread — guarded by
    // venue_mu_ which only wraps these three fields. None of the other
    // ExitManager containers are touched off-thread.
    struct exchange_leg
    {
        std::uint64_t opener_order_id = 0;
        std::string   strategy_name;
    };
    std::shared_ptr<IBracketAdapter> bracket_adapter_;
    std::unordered_map<std::uint64_t, bracket_handles> handles_;
    std::unordered_map<std::string, exchange_leg>      exchange_to_leg_;
    mutable std::mutex venue_mu_;

    void untrack_opener(std::uint64_t opener_order_id,
                        const std::string& strategy_name,
                        const std::string& symbol);

    // Single point that drops handles_ + exchange_to_opener_ entries and
    // tells the adapter to clean up venue-side. Called from every cancel
    // path so adapter.cancel() is invoked exactly once per opener.
    void release_venue_bracket(std::uint64_t opener_order_id);
};

}
