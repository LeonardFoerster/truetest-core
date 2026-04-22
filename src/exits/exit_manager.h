#pragma once

#include "core/event.h"
#include "exits/exit_intent.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace truetest::exits {

// Engine-side enforcement of strategy-declared exit intents. One instance
// per engine. Intents are keyed by (strategy_name, symbol) so multi-strategy
// runs that both trade the same symbol each carry their own stops.
//
// Lifecycle:
//   1. Strategy returns an exit_intent alongside an entry order.
//   2. Engine calls register_pending(intent) immediately after submitting
//      the entry.
//   3. When the opener fill arrives, engine calls on_fill(fill). The
//      manager promotes the pending intent to "armed" using the actual
//      fill price as the entry reference (for trailing) and the actual
//      fill qty as the close qty.
//   4. Every price event (bar close, trade tick, L2 mid refresh) the
//      engine calls on_price(sym, px, ts). If any trigger fires, the
//      manager returns an order_event and the engine submits it via its
//      normal process_order path — so shadow mode's TradeTapeShadowAdapter
//      sees the exit the same way it sees any strategy order, and
//      realised P&L / fees end up in the portfolio + analytics.
//   5. cancel(strategy, symbol) removes any intent (e.g. when the
//      strategy's own signal-based exit fires first).
class ExitManager
{
public:
    ExitManager() = default;

    // Strategy just emitted an entry; remember what it wants to happen
    // when we fill. One pending intent per (strategy, symbol) — a new
    // call replaces any earlier pending.
    void register_pending(exit_intent intent);

    // Bind pending → armed on opener fill. The fill price becomes the
    // entry reference for trailing; the fill qty overrides the intent qty
    // (which was only a hint based on intended_price). If the fill has
    // no matching pending intent, this is a no-op.
    void on_fill(const fill_event& f);

    // Evaluate every armed intent for `symbol` at price `px`; the first
    // one to trigger wins this call (rare to have >1 strategy on the same
    // symbol, but deterministic if it happens). Returns the synthetic
    // market close order on fire; nullopt otherwise. The manager erases
    // the armed intent atomically so a second trigger on the same tick
    // doesn't double-fire.
    std::optional<order_event> on_price(const std::string& symbol,
                                        double px,
                                        std::chrono::system_clock::time_point ts);

    // Drop pending+armed for (strategy, symbol) — used when the strategy
    // itself exits the position via its signal path, and when a manual
    // flatten happens.
    void cancel(const std::string& strategy_name, const std::string& symbol);

    // Diagnostics.
    std::size_t armed_count() const { return armed_.size(); }
    std::size_t pending_count() const { return pending_.size(); }

private:
    struct key_hash
    {
        std::size_t operator()(const std::pair<std::string, std::string>& k) const noexcept
        {
            std::hash<std::string> h;
            return h(k.first) ^ (h(k.second) << 1);
        }
    };
    using key_t = std::pair<std::string, std::string>;  // (strategy, symbol)

    struct armed_intent
    {
        exit_intent   intent;
        double        entry_price = 0.0;   // bound at opener fill
        double        best_price  = 0.0;   // running MFE for trailing
    };

    std::unordered_map<key_t, exit_intent,   key_hash> pending_;
    std::unordered_map<key_t, armed_intent,  key_hash> armed_;

    // Tracks order_id → key for on_fill lookup. Pending intents are
    // tagged with their opener_order_id by the engine, so this is a
    // direct reverse map.
    std::unordered_map<std::uint64_t, key_t> opener_id_to_key_;
};

} // namespace truetest::exits
