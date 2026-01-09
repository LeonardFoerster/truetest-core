#include "sma.h"
#include "../pricing/black_scholes.h"
#include "../core/event.h"

#include <iostream>
#include <optional>

strategy::strategy(std::size_t period) : sma_(period) {}

std::optional<signal_event> strategy::on_market(const market_event& mkt)
{
    auto sma_value = sma_.update(mkt.get_close());
    if (!sma_value) return std::nullopt; 

    if (!position_open_ && mkt.get_close() > *sma_value) {
        return signal_event(mkt.get_timestamp(), mkt.get_symbol(), signal_type::buy, 1.0);
    }
    if (position_open_ && mkt.get_close() < *sma_value) {
        return signal_event(mkt.get_timestamp(), mkt.get_symbol(), signal_type::sell, 1.0);
    }
    return std::nullopt; 
}

void strategy::set_position_open(bool open)
{
    position_open_ = open;
}

