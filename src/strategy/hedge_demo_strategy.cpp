#include "hedge_demo_strategy.h"
#include "strategy_registry.h"
#include "../core/event.h"

REGISTER_STRATEGY("hedge-demo", []() {
    return std::make_shared<hedge_demo_strategy>();
})

hedge_demo_strategy::hedge_demo_strategy(std::size_t hedge_gap,
                                         double notional,
                                         double sl_pct,
                                         double tp_pct)
    : hedge_gap_(hedge_gap), notional_(notional),
      sl_pct_(sl_pct), tp_pct_(tp_pct) {}

std::optional<order_event> hedge_demo_strategy::on_market(const market_event& mkt)
{
    const std::string& sym = mkt.get_symbol();
    double price = mkt.get_close();
    if (price <= 0.0) return std::nullopt;

    double qty = notional_ / price;
    long& bar_counter = bars_since_long_entry_[sym];

    // Phase 1: open the long leg the first time we see this symbol.
    if (long_open_[sym] == 0 && bar_counter == 0)
    {
        pending_intents_.clear();
        pending_intents_.push_back(
            truetest::exits::make_long_exit_intent(
                sym, price, qty, sl_pct_, tp_pct_, "hedge-demo"));
        bar_counter = 1;
        return order_event(mkt.get_timestamp(), sym,
                           order_type::market, order_side::buy, qty, price);
    }

    // Phase 2: hedge_gap bars after the long, open the short leg so both
    // legs are live simultaneously. The lot table and ExitManager track
    // them independently even though the netted book sees zero exposure.
    if (long_open_[sym] > 0 && short_open_[sym] == 0 &&
        bar_counter >= static_cast<long>(hedge_gap_))
    {
        pending_intents_.clear();
        pending_intents_.push_back(
            truetest::exits::make_short_exit_intent(
                sym, price, qty, sl_pct_, tp_pct_, "hedge-demo"));
        return order_event(mkt.get_timestamp(), sym,
                           order_type::market, order_side::sell, qty, price);
    }

    if (bar_counter > 0) ++bar_counter;
    return std::nullopt;
}

std::vector<truetest::exits::exit_intent>
hedge_demo_strategy::take_pending_exit_intents()
{
    auto out = std::move(pending_intents_);
    pending_intents_.clear();
    return out;
}

void hedge_demo_strategy::on_fill(const fill_event& fill,
                                  std::uint64_t opener_order_id)
{
    const bool is_opener = (opener_order_id == fill.get_order_id());
    const std::string& sym = fill.get_symbol();

    if (is_opener)
    {
        opener_sides_[opener_order_id] = fill.get_side();
        if (fill.get_side() == order_side::buy)
            ++long_open_[sym];
        else
            ++short_open_[sym];
        return;
    }

    // Closer: decrement the counter matching the OPENER's side (not this
    // fill's side - a closer for a long is a sell, and vice versa).
    auto it = opener_sides_.find(opener_order_id);
    if (it == opener_sides_.end()) return;
    if (it->second == order_side::buy)
        long_open_[sym]  = std::max(0, long_open_[sym]  - 1);
    else
        short_open_[sym] = std::max(0, short_open_[sym] - 1);
    opener_sides_.erase(it);
}
