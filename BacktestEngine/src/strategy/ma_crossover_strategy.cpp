#include "ma_crossover_strategy.h"
#include "../core/event.h"

#include <optional>

ma_crossover_strategy::ma_crossover_strategy(std::size_t period) : sma_(period) {}

std::optional<order_event> ma_crossover_strategy::on_market(const market_event& mkt)
{
    auto sma_value = sma_.update(mkt.get_close());
    if (!sma_value) return std::nullopt;

    if (!position_open_ && mkt.get_close() > *sma_value) {
        return order_event(mkt.get_timestamp(), mkt.get_symbol(), order_type::limit, order_side::buy, 100, mkt.get_close());
    }
    if (position_open_ && mkt.get_close() < *sma_value) {
        return order_event(mkt.get_timestamp(), mkt.get_symbol(), order_type::limit, order_side::sell, 100, mkt.get_close());
    }
    return std::nullopt;
}

void ma_crossover_strategy::set_position_open(bool open)
{
    position_open_ = open;
}
