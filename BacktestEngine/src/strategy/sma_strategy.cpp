#include "sma_strategy.h"
#include "../core/event.h"

#include <iostream>
#include <optional>

sma_strategy::sma_strategy(std::size_t period) : sma_(period) {}

std::optional<order_event> sma_strategy::on_market(const market_event& mkt)
{
    auto sma_value = sma_.update(mkt.get_close());
    if (!sma_value) {
        std::cout << "SMA not ready" << std::endl;
        return std::nullopt; 
    }

    std::cout << "SMA: " << *sma_value << ", Price: " << mkt.get_close() << ", Position: " << position_open_ << std::endl;

    if (!position_open_ && mkt.get_close() > *sma_value) {
        return order_event(mkt.get_timestamp(), mkt.get_symbol(), order_type::limit, order_side::buy, 100, mkt.get_close());
    }
    if (position_open_ && mkt.get_close() < *sma_value) {
        std::cout << "Sell signal generated" << std::endl;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(), order_type::limit, order_side::sell, 100, mkt.get_close());
    }
    return std::nullopt; 
}

void sma_strategy::set_position_open(bool open)
{
    position_open_ = open;
}