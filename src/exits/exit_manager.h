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

// Engine-side enforcement of strategy-declared exit intents. Keyed by
// (strategy, symbol) so multi-strategy runs on the same symbol carry
// independent stops. Lifecycle: register_pending at entry submit → on_fill
// promotes to armed (using actual fill px/qty) → on_price evaluates every
// tick and returns a synthetic close order when any trigger crosses.
class ExitManager
{
public:
    ExitManager() = default;

    // New call replaces any earlier pending intent for the same key.
    void register_pending(exit_intent intent);

    // Binds pending → armed using the fill price (trailing reference) and
    // fill qty (the intent's qty was just a hint). No-op if no match.
    void on_fill(const fill_event& f);

    // First armed intent to trigger wins and is erased atomically so a
    // second crossing on the same tick can't double-fire.
    std::optional<order_event> on_price(const std::string& symbol,
                                        double px,
                                        std::chrono::system_clock::time_point ts);

    // Drops pending+armed when the strategy exits via its own signal path.
    void cancel(const std::string& strategy_name, const std::string& symbol);

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
    using key_t = std::pair<std::string, std::string>;

    struct armed_intent
    {
        exit_intent   intent;
        double        entry_price = 0.0;
        double        best_price  = 0.0;  // running MFE for trailing
    };

    // Multimap lets a strategy declare a TP1/TP2/SL scale-out plan per key.
    std::unordered_multimap<key_t, exit_intent,   key_hash> pending_;
    std::unordered_multimap<key_t, armed_intent,  key_hash> armed_;

    std::unordered_map<std::uint64_t, key_t> opener_id_to_key_;
};

} // namespace truetest::exits
