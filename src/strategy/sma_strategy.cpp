#include "sma_strategy.h"
#include "strategy_registry.h"
#include "../core/event.h"

#include <optional>

REGISTER_STRATEGY("sma", []() {
    return std::make_shared<sma_strategy>();
})

sma_strategy::sma_strategy(std::size_t period) : period_(period) {}

simple_moving_average& sma_strategy::get_sma(const std::string& symbol)
{
    auto it = smas_.find(symbol);
    if (it == smas_.end())
    {
        smas_.emplace(symbol, simple_moving_average(period_));
        return smas_.at(symbol);
    }
    return it->second;
}

std::optional<order_event> sma_strategy::on_market(const market_event& mkt)
{
    auto& sma = get_sma(mkt.get_symbol());
    auto sma_value = sma.update(mkt.get_close());
    if (!sma_value) return std::nullopt;

    bool is_open = position_open_[mkt.get_symbol()];
    const order_type otype = order_type_for_fill_style();
    const double ref_px = mkt.get_close(); // signal reference; market fills at book mid/open

    if (!is_open && mkt.get_close() > *sma_value) {
        // Optimistic gate: block free-fire until fill or engine resync on reject.
        position_open_[mkt.get_symbol()] = true;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(), otype, order_side::buy,
                           100.0, ref_px);
    }
    if (is_open && mkt.get_close() < *sma_value) {
        position_open_[mkt.get_symbol()] = false;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(), otype, order_side::sell,
                           100.0, ref_px);
    }
    return std::nullopt;
}

void sma_strategy::set_position_open(const std::string& symbol, bool open)
{
    position_open_[symbol] = open;
}
