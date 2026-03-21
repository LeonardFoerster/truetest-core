#pragma once
#include "../core/event.h"
#include <optional>
#include <string>

class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual std::optional<order_event> on_market(const market_event& mkt) = 0;
    virtual std::optional<order_event> on_tick(const tick_event&) { return std::nullopt; }
    virtual std::optional<order_event> on_l2_update(const l2_update_event&) { return std::nullopt; }

    // Per-symbol position tracking
    virtual void set_position_open(const std::string& symbol, bool open) = 0;

    // Legacy single-symbol convenience (delegates to per-symbol with empty string)
    virtual void set_position_open(bool open) { set_position_open("", open); }
};
