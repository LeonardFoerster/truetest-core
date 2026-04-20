#include "ma_crossover_strategy.h"
#include "strategy_registry.h"
#include "../core/event.h"

#include <optional>

REGISTER_STRATEGY("ma-crossover", []() {
    return std::make_shared<ma_crossover_strategy>();
})

ma_crossover_strategy::ma_crossover_strategy(std::size_t fast_period, std::size_t slow_period)
    : fast_period_(fast_period), slow_period_(slow_period) {}

simple_moving_average& ma_crossover_strategy::get_fast_sma(const std::string& symbol)
{
    auto it = fast_smas_.find(symbol);
    if (it == fast_smas_.end())
    {
        fast_smas_.emplace(symbol, simple_moving_average(fast_period_));
        return fast_smas_.at(symbol);
    }
    return it->second;
}

simple_moving_average& ma_crossover_strategy::get_slow_sma(const std::string& symbol)
{
    auto it = slow_smas_.find(symbol);
    if (it == slow_smas_.end())
    {
        slow_smas_.emplace(symbol, simple_moving_average(slow_period_));
        return slow_smas_.at(symbol);
    }
    return it->second;
}

std::optional<order_event> ma_crossover_strategy::on_market(const market_event& mkt)
{
    auto& fast = get_fast_sma(mkt.get_symbol());
    auto& slow = get_slow_sma(mkt.get_symbol());
    auto fast_val = fast.update(mkt.get_close());
    auto slow_val = slow.update(mkt.get_close());
    if (!fast_val || !slow_val) return std::nullopt;

    bool fast_above = *fast_val > *slow_val;
    bool is_open = position_open_[mkt.get_symbol()];

    auto prev_it = prev_fast_above_.find(mkt.get_symbol());
    if (prev_it == prev_fast_above_.end())
    {
        prev_fast_above_[mkt.get_symbol()] = fast_above;
        return std::nullopt;
    }

    bool was_above = prev_it->second;
    prev_it->second = fast_above;

    if (!is_open && fast_above && !was_above)
    {
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::limit, order_side::buy, 100.0, mkt.get_close());
    }
    if (is_open && !fast_above && was_above)
    {
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::limit, order_side::sell, 100.0, mkt.get_close());
    }
    return std::nullopt;
}

void ma_crossover_strategy::set_position_open(const std::string& symbol, bool open)
{
    position_open_[symbol] = open;
}
