#include "sma_strategy.h"
#include "../strategy_registry.h"
#include "../../core/event.h"
#include "../../execution/position_sizing.h"

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

double sma_strategy::size_long(double entry) const
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
    in.entry_fee_rate = entry_fee_rate_;
    in.exit_fee_rate = exit_fee_rate_;
    in.max_notional_frac = 1.0; // never exceed full equity notional
    return truetest::risk::compute_risk_quantity(in);
}

std::optional<order_event> sma_strategy::on_market(const market_event& mkt)
{
    auto& sma = get_sma(mkt.get_symbol());
    auto sma_value = sma.update(mkt.get_close());
    if (!sma_value) return std::nullopt;

    bool is_open = position_open_[mkt.get_symbol()];
    const order_type otype = order_type_for_fill_style();
    const double ref_px = mkt.get_close();

    if (!is_open && mkt.get_close() > *sma_value) {
        const double qty = size_long(ref_px);
        if (!(qty > 0.0)) return std::nullopt;
        position_open_[mkt.get_symbol()] = true;
        position_qty_[mkt.get_symbol()] = qty;
        pending_intents_.push_back(
            truetest::exits::make_long_exit_intent(
                mkt.get_symbol(), ref_px, qty, sl_pct_, tp_pct_, "sma"));
        return order_event(mkt.get_timestamp(), mkt.get_symbol(), otype, order_side::buy,
                           qty, ref_px);
    }
    if (is_open && mkt.get_close() < *sma_value) {
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
        return order_event(mkt.get_timestamp(), mkt.get_symbol(), otype, order_side::sell,
                           qty, ref_px);
    }
    return std::nullopt;
}

void sma_strategy::set_position_open(const std::string& symbol, bool open)
{
    position_open_[symbol] = open;
    if (!open) {
        position_qty_[symbol] = 0.0;
        opener_filled_[symbol] = 0.0;
    }
}

std::vector<truetest::exits::exit_intent> sma_strategy::take_pending_exit_intents()
{
    std::vector<truetest::exits::exit_intent> out;
    out.swap(pending_intents_);
    return out;
}
