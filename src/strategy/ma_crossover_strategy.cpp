#include "ma_crossover_strategy.h"
#include "strategy_registry.h"
#include "../core/event.h"
#include "../execution/position_sizing.h"

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

double ma_crossover_strategy::size_long(double entry) const
{
    if (!(entry > 0.0) || !(equity_ > 0.0) || !(risk_fraction_ > 0.0))
        return 0.0;
    const double sl_dist = (sl_pct_ > 0.0) ? sl_pct_ : 0.003;
    truetest::risk::risk_size_inputs in;
    in.equity = equity_;
    in.risk_fraction = risk_fraction_;
    in.entry_price = entry;
    in.stop_price = entry * (1.0 - sl_dist);
    in.is_long = true;
    in.max_notional_frac = 1.0;
    return truetest::risk::compute_risk_quantity(in);
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

    const order_type otype = order_type_for_fill_style();
    const double ref_px = mkt.get_close();

    if (!is_open && fast_above && !was_above)
    {
        const double qty = size_long(ref_px);
        if (!(qty > 0.0)) return std::nullopt;
        position_open_[mkt.get_symbol()] = true;
        position_qty_[mkt.get_symbol()] = qty;
        pending_intents_.push_back(
            truetest::exits::make_long_exit_intent(
                mkt.get_symbol(), ref_px, qty, sl_pct_, tp_pct_, "ma-crossover"));
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           otype, order_side::buy, qty, ref_px);
    }
    if (is_open && !fast_above && was_above)
    {
        // Prefer qty stored on entry; if engine only resynced the boolean gate
        // (no fill-path qty), fall back to risk size at the signal price so
        // the exit still fires.
        double qty = position_qty_[mkt.get_symbol()];
        if (!(qty > 0.0))
            qty = size_long(ref_px);
        if (!(qty > 0.0)) {
            position_open_[mkt.get_symbol()] = false;
            return std::nullopt;
        }
        position_open_[mkt.get_symbol()] = false;
        position_qty_[mkt.get_symbol()] = 0.0;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           otype, order_side::sell, qty, ref_px);
    }
    return std::nullopt;
}

void ma_crossover_strategy::set_position_open(const std::string& symbol, bool open)
{
    position_open_[symbol] = open;
    if (!open) position_qty_[symbol] = 0.0;
}

std::vector<truetest::exits::exit_intent> ma_crossover_strategy::take_pending_exit_intents()
{
    std::vector<truetest::exits::exit_intent> out;
    out.swap(pending_intents_);
    return out;
}
