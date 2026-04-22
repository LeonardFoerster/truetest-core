#pragma once
#include "../core/event.h"
#include "exits/exit_intent.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <limits>
#include <stdexcept>

struct param_def
{
    std::string name;
    double default_value = 0.0;
    double min_value = -std::numeric_limits<double>::max();
    double max_value = std::numeric_limits<double>::max();
    std::string description;
};

class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual std::optional<order_event> on_market(const market_event& mkt) = 0;
    virtual std::optional<order_event> on_tick(const tick_event&) { return std::nullopt; }
    virtual std::optional<order_event> on_l2_update(const l2_update_event&) { return std::nullopt; }

    virtual void set_position_open(const std::string& symbol, bool open) = 0;
    virtual void set_position_open(bool open) { set_position_open("", open); }

    // Strategy-declared exit plan (SL/TP/trailing/time). Returned at most
    // once per entry order: the engine polls this right after each
    // on_market/on_tick/on_l2_update call, registers the intent with its
    // ExitManager, and the manager owns enforcement from that point on.
    // Default: no exit plan (strategy handles its own exits via signals).
    virtual std::optional<truetest::exits::exit_intent> take_pending_exit_intent()
    {
        return std::nullopt;
    }

    virtual std::vector<param_def> get_param_schema() const { return {}; }

    virtual void set_param(const std::string& key, double value)
    {
        (void)key; (void)value;
        throw std::runtime_error("Unknown parameter: " + key);
    }

    virtual std::vector<std::pair<std::string, double>> get_indicator_values(
        const std::string& /*symbol*/) const
    {
        return {};
    }
};
