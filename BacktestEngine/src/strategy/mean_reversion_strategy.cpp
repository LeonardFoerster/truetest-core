#include "mean_reversion_strategy.h"
#include "../core/event.h"

#include <cmath>
#include <optional>

mean_reversion_strategy::mean_reversion_strategy(std::size_t period, double equity,
                                                   double risk_fraction, double sl_pct, double tp_pct)
    : period_(period), equity_(equity), risk_fraction_(risk_fraction),
      sl_pct_(sl_pct), tp_pct_(tp_pct) {}

simple_moving_average& mean_reversion_strategy::get_sma(const std::string& symbol)
{
    auto it = smas_.find(symbol);
    if (it == smas_.end())
    {
        smas_.emplace(symbol, simple_moving_average(period_));
        return smas_.at(symbol);
    }
    return it->second;
}

double mean_reversion_strategy::compute_quantity(double price) const
{
    if (price <= 0.0) return 0.0;
    return equity_ * risk_fraction_ / price;
}

std::optional<order_event> mean_reversion_strategy::on_market(const market_event& mkt)
{
    auto& sma = get_sma(mkt.get_symbol());
    auto sma_value = sma.update(mkt.get_close());
    if (!sma_value) return std::nullopt;

    bool is_open = position_open_[mkt.get_symbol()];
    double qty = compute_quantity(mkt.get_close());
    if (qty <= 0.0) return std::nullopt;

    if (!is_open && mkt.get_close() < *sma_value) {
        return order_event(mkt.get_timestamp(), mkt.get_symbol(), order_type::market, order_side::buy, qty, mkt.get_close());
    }
    if (is_open && mkt.get_close() > *sma_value) {
        return order_event(mkt.get_timestamp(), mkt.get_symbol(), order_type::market, order_side::sell, qty, mkt.get_close());
    }
    return std::nullopt;
}

std::optional<order_event> mean_reversion_strategy::on_tick(const tick_event& te)
{
    // Check SL/TP first — these take priority over new signals
    auto stop_order = check_stops(te.get_symbol(), te.get_price(), te.get_timestamp());
    if (stop_order) return stop_order;

    auto& sma = get_sma(te.get_symbol());
    auto sma_value = sma.update(te.get_price());
    if (!sma_value) return std::nullopt;

    bool is_open = position_open_[te.get_symbol()];
    double qty = compute_quantity(te.get_price());
    if (qty <= 0.0) return std::nullopt;

    if (!is_open && te.get_price() < *sma_value) {
        // Set SL/TP for the new position
        double entry = te.get_price();
        set_stops(te.get_symbol(),
                  entry * (1.0 - sl_pct_),   // SL below entry
                  entry * (1.0 + tp_pct_),    // TP above entry
                  qty);
        return order_event(te.get_timestamp(), te.get_symbol(), order_type::market, order_side::buy, qty, entry);
    }
    if (is_open && te.get_price() > *sma_value) {
        clear_stops(te.get_symbol());
        return order_event(te.get_timestamp(), te.get_symbol(), order_type::market, order_side::sell, qty, te.get_price());
    }
    return std::nullopt;
}

void mean_reversion_strategy::set_position_open(const std::string& symbol, bool open)
{
    position_open_[symbol] = open;
    if (!open)
        clear_stops(symbol);
}
