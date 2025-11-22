#pragma once
#include "../core/event.h"
#include "../indicator/sma.h"

#include <optional>

class strategy
{
public:
    explicit strategy(std::size_t period = 10);
    std::optional<signal_event> on_market(const market_event& mkt); // Event-driven signal generation
    void set_position_open(bool open); // Syncs position state from portfolio

private:
    simple_moving_average sma_;
    bool position_open_ = false; // Tracks if a position is currently open
};
