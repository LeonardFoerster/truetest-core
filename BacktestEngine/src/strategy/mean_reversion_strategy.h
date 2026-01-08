#pragma once
#include "../core/event.h"
#include "../indicator/sma.h"

#include <optional>

class mean_reversion_strategy
{
public:
    explicit mean_reversion_strategy(std::size_t period = 20);
    std::optional<signal_event> on_market(const market_event& mkt);
    void set_position_open(bool open);

private:
    simple_moving_average sma_;
    bool position_open_ = false;
};
