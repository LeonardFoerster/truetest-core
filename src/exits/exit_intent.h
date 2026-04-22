#pragma once

#include "core/event.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace truetest::exits {

enum class exit_reason : std::uint8_t
{
    stop_loss,
    take_profit,
    trailing_stop,
    time_stop,
    manual
};

// Declarative exit plan produced by a strategy when it enters. The engine
// owns enforcement: it binds the intent to the actual opener fill (so the
// entry price is the real fill_price, not the intended price), evaluates
// triggers on every market/tick/l2 update, and submits the close order
// through the normal engine pipeline so both sim- and exchange-side
// adapters see it. A strategy never executes its own stops anymore.
struct exit_intent
{
    std::string symbol;
    order_side  close_side = order_side::sell;
    double      qty = 0.0;

    std::optional<double> stop_loss;
    std::optional<double> take_profit;

    // Fraction of the running best price; e.g. 0.005 ⇒ 0.5% trailing.
    // When set, the manager raises stop_loss each tick by
    //   stop_loss = max(stop_loss, best_price * (1 - trailing_pct))
    // for longs (and the mirror for shorts).
    std::optional<double> trailing_pct;

    // Absolute deadline. If the current event timestamp exceeds this, the
    // manager fires a time_stop exit at the event's price.
    std::optional<std::chrono::system_clock::time_point> deadline;

    // Bound by the engine when the opener fill arrives.
    std::uint64_t opener_order_id = 0;

    // Human-readable tag for the exit's source (strategy name), carried
    // through to Analytics for per-strategy attribution.
    std::string strategy_name;
};

} // namespace truetest::exits
