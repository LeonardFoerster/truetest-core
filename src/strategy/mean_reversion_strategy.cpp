#include "mean_reversion_strategy.h"
#include "strategy_registry.h"
#include "../core/event.h"

#include <cmath>
#include <optional>

REGISTER_STRATEGY("mean-reversion", []() {
    return std::make_shared<mean_reversion_strategy>();
})

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

// Exits are owned by the engine's ExitManager via the bracket registered
// at entry — there is intentionally no signal-based SELL here. A previous
// version closed when price crossed back above the SMA, but that always
// fired before TP and competed with SL, leaving the bracket effectively
// dead. SL and TP now behave as independent triggers per entry.
std::optional<order_event> mean_reversion_strategy::on_market(const market_event& mkt)
{
    auto& sma = get_sma(mkt.get_symbol());
    auto sma_value = sma.update(mkt.get_close());
    if (!sma_value) return std::nullopt;

    if (position_open_[mkt.get_symbol()])  return std::nullopt;
    if (mkt.get_close() >= *sma_value)     return std::nullopt;

    double qty = compute_quantity(mkt.get_close());
    if (qty <= 0.0) return std::nullopt;

    double entry = mkt.get_close();
    pending_intent_ = truetest::exits::make_long_exit_intent(
        mkt.get_symbol(), entry, qty, sl_pct_, tp_pct_, "mean-reversion");
    return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                       order_type::market, order_side::buy, qty, entry);
}

std::optional<order_event> mean_reversion_strategy::on_tick(const tick_event& te)
{
    auto& sma = get_sma(te.get_symbol());
    auto sma_value = sma.update(te.get_price());
    if (!sma_value) return std::nullopt;

    if (position_open_[te.get_symbol()]) return std::nullopt;
    if (te.get_price() >= *sma_value)    return std::nullopt;

    double qty = compute_quantity(te.get_price());
    if (qty <= 0.0) return std::nullopt;

    double entry = te.get_price();
    pending_intent_ = truetest::exits::make_long_exit_intent(
        te.get_symbol(), entry, qty, sl_pct_, tp_pct_, "mean-reversion");
    return order_event(te.get_timestamp(), te.get_symbol(),
                       order_type::market, order_side::buy, qty, entry);
}

std::optional<truetest::exits::exit_intent>
mean_reversion_strategy::take_pending_exit_intent()
{
    auto out = std::move(pending_intent_);
    pending_intent_.reset();
    return out;
}

void mean_reversion_strategy::set_position_open(const std::string& symbol, bool open)
{
    position_open_[symbol] = open;
}
