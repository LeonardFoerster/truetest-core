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

// Declarative exit plan emitted by a strategy at entry. The engine owns
// enforcement and binds triggers to the actual opener fill price, so a
// strategy never executes its own stops.
struct exit_intent
{
    std::string symbol;
    order_side  close_side = order_side::sell;
    double      qty = 0.0;

    // Fraction of opener fill to close. 1.0 = full exit. Multiple intents
    // per opener compose for TP1/TP2/SL scale-outs; risk layer catches
    // fractions summing above 1.0.
    double qty_fraction = 1.0;

    std::optional<double> stop_loss;
    std::optional<double> take_profit;

    // Fraction of best price (0.005 = 0.5% trail). Raises stop_loss each
    // tick to max(stop_loss, best * (1 - trailing_pct)) for longs.
    std::optional<double> trailing_pct;

    std::optional<std::chrono::system_clock::time_point> deadline;

    std::uint64_t opener_order_id = 0;

    // Carried through to Analytics for per-strategy attribution.
    std::string strategy_name;
};

// Shared helpers so strategies don't each re-derive the SL/TP math with the
// wrong sign for shorts.
inline exit_intent make_long_exit_intent(const std::string& symbol,
                                         double entry, double qty,
                                         double sl_pct, double tp_pct,
                                         const std::string& strategy_name = {})
{
    exit_intent ei;
    ei.symbol        = symbol;
    ei.close_side    = order_side::sell;
    ei.qty           = qty;
    if (sl_pct > 0.0) ei.stop_loss   = entry * (1.0 - sl_pct);
    if (tp_pct > 0.0) ei.take_profit = entry * (1.0 + tp_pct);
    ei.strategy_name = strategy_name;
    return ei;
}

inline exit_intent make_short_exit_intent(const std::string& symbol,
                                          double entry, double qty,
                                          double sl_pct, double tp_pct,
                                          const std::string& strategy_name = {})
{
    exit_intent ei;
    ei.symbol        = symbol;
    ei.close_side    = order_side::buy;
    ei.qty           = qty;
    if (sl_pct > 0.0) ei.stop_loss   = entry * (1.0 + sl_pct);
    if (tp_pct > 0.0) ei.take_profit = entry * (1.0 - tp_pct);
    ei.strategy_name = strategy_name;
    return ei;
}

}
