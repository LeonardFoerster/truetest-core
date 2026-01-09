#pragma once
#include "../core/event.h"
#include <optional>

class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual std::optional<order_event> on_market(const market_event& mkt) = 0;
    virtual void set_position_open(bool open) = 0;
};