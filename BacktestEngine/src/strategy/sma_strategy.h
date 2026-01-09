#pragma once
#include "../core/event.h"
#include "../indicator/sma.h"
#include "strategy_interface.h"

#include <optional>

class sma_strategy : public IStrategy
{
public:
    explicit sma_strategy(std::size_t period = 20);
    std::optional<order_event> on_market(const market_event& mkt) override;
    void set_position_open(bool open) override;

private:
    simple_moving_average sma_;
    bool position_open_ = false;
};