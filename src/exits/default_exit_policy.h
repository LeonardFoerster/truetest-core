#pragma once

#include "core/event.h"
#include "exits/exit_intent.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace truetest::exits {

// Platform-owned protective exits. Strategies need not implement SL/TP;
// the engine applies this policy after draining strategy intents.
enum class exit_policy_mode : std::uint8_t
{
    floor = 0,           // ensure min SL; fill empty strategy plans
    strategy_only = 1,   // only strategy intents (legacy / research)
    engine_only = 2,     // discard strategy intents; platform only
    union_mode = 3,      // keep strategy intents; append globally missing platform legs
};

struct default_exit_params
{
    exit_policy_mode mode = exit_policy_mode::floor;
    double sl_pct = 0.0;      // 0 = off / strategy-defined
    double tp_pct = 0.0;      // 0 = off / strategy-defined
    double trail_pct = 0.0;   // 0 = off; fraction of best price for trailing
};

// True when the order reduces |net| (signal close / scale-out), so platform
// defaults must not arm a new inverted entry bracket.
bool is_position_reducing(const order_event& order, double net_qty);

// True when any intent already carries a stop_loss.
bool intents_have_stop_loss(const std::vector<exit_intent>& intents);

// True when any intent already carries a take_profit.
bool intents_have_take_profit(const std::vector<exit_intent>& intents);

// True when any intent already carries a trailing_pct.
bool intents_have_trail(const std::vector<exit_intent>& intents);

// Build a single platform intent for an entry order (long or short).
// Empty optional fields when the corresponding pct is <= 0.
// Returns nullopt if both SL and TP disabled and trail off, or qty/price invalid.
std::optional<exit_intent> make_platform_exit_intent(const order_event& order,
                                                     const default_exit_params& p);

// Merge strategy intents with platform defaults per mode.
// net_qty: portfolio net for the symbol *before* this order fills.
// strategy_intents: already drained from the strategy (may be empty).
// Does not stamp opener_order_id / strategy_name (engine does that).
std::vector<exit_intent> apply_default_exit_policy(
    const default_exit_params& p,
    const order_event& order,
    double net_qty,
    std::vector<exit_intent> strategy_intents);

// Parse CLI/config token; unknown → nullopt.
std::optional<exit_policy_mode> parse_exit_policy_mode(std::string_view s);

const char* exit_policy_mode_name(exit_policy_mode m);

} // namespace truetest::exits
