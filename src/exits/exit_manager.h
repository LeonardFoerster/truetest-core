#pragma once

#include "core/event.h"
#include "exits/exit_intent.h"

#include <chrono>
#include <cstdint>
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

private:
    struct armed_intent
    {
        exit_intent intent;
        double      entry_price = 0.0;
        double      best_price  = 0.0;  // running MFE for trailing
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

    void untrack_opener(std::uint64_t opener_order_id,
                        const std::string& strategy_name,
                        const std::string& symbol);
};

} // namespace truetest::exits
