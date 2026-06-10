#include "larry_connor_strategy.h"
#include "strategy_registry.h"
#include "../core/event.h"

#include <optional>

REGISTER_STRATEGY("larry_connor", []() {
    return std::make_shared<larry_connor_strategy>();
})

larry_connor_strategy::larry_connor_strategy(std::size_t ma_period,
                                             std::size_t entry_period,
                                             std::size_t exit_period,
                                             std::size_t atr_period,
                                             double equity,
                                             double risk_fraction)
    : ma_period_(ma_period)
    , entry_period_(entry_period)
    , exit_period_(exit_period)
    , atr_period_(atr_period)
    , equity_(equity)
    , risk_fraction_(risk_fraction)
{
}

larry_connor_strategy::SymbolState&
larry_connor_strategy::get_state(const std::string& symbol)
{
    auto it = states_.find(symbol);
    if (it == states_.end())
    {
        auto [ins, _] = states_.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(symbol),
            std::forward_as_tuple(ma_period_, entry_period_, exit_period_, atr_period_));
        return ins->second;
    }
    return it->second;
}

double larry_connor_strategy::compute_quantity(double price) const
{
    if (price <= 0.0) return 0.0;
    return equity_ * risk_fraction_ / price;
}

std::optional<order_event> larry_connor_strategy::on_market(const market_event& mkt)
{
    auto& st = get_state(mkt.get_symbol());

    const double close = mkt.get_close();

    // Update all indicators every bar (regime, entry/exit triggers, ATR).
    auto ma_value = st.ma.update(close);
    st.entry_low.update(close);
    st.exit_high.update(close);
    (void)st.atr.update(mkt.get_high(), mkt.get_low(), close);

    // Need the regime filter and at least one rolling window ready before acting.
    if (!ma_value || !st.entry_low.ready() || !st.exit_high.ready())
        return std::nullopt;

    if (st.position_open)
    {
        // LongExit: close prints a fresh exit_period-bar high.
        if (close >= st.exit_high.max())
        {
            double qty = st.open_qty;
            st.position_open = false;
            st.open_qty = 0.0;
            if (qty <= 0.0) return std::nullopt;
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::sell, qty, close);
        }
        return std::nullopt;
    }

    // LongEntry: bullish regime (close above MA) and a fresh entry_period-bar low.
    if (close > *ma_value && close <= st.entry_low.min())
    {
        double qty = compute_quantity(close);
        if (qty <= 0.0) return std::nullopt;
        st.position_open = true;
        st.open_qty = qty;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy, qty, close);
    }

    return std::nullopt;
}

void larry_connor_strategy::set_position_open(const std::string& symbol, bool open)
{
    // Keep our flag in sync with the engine's netted view of the book. The
    // engine flips this when the symbol moves between flat and non-flat after a
    // fill; honouring the flat signal guards against a missed/partial close.
    auto& st = get_state(symbol);
    st.position_open = open;
    if (!open)
        st.open_qty = 0.0;
}

std::vector<param_def> larry_connor_strategy::get_param_schema() const
{
    return {
        {"ma_period", static_cast<double>(ma_period_), 1, 10000, "Regime-filter SMA period (MA200)"},
        {"entry_period", static_cast<double>(entry_period_), 1, 1000, "Rolling-low lookback for entry (7D_Low)"},
        {"exit_period", static_cast<double>(exit_period_), 1, 1000, "Rolling-high lookback for exit (7D_High)"},
        {"atr_period", static_cast<double>(atr_period_), 1, 1000, "Wilder's ATR period"},
        {"equity", equity_, 0, 1e18, "Account equity for position sizing"},
        {"risk_fraction", risk_fraction_, 0, 1, "Fraction of equity per trade"},
    };
}

void larry_connor_strategy::set_param(const std::string& key, double value)
{
    if (key == "ma_period") { ma_period_ = static_cast<std::size_t>(value); states_.clear(); }
    else if (key == "entry_period") { entry_period_ = static_cast<std::size_t>(value); states_.clear(); }
    else if (key == "exit_period") { exit_period_ = static_cast<std::size_t>(value); states_.clear(); }
    else if (key == "atr_period") { atr_period_ = static_cast<std::size_t>(value); states_.clear(); }
    else if (key == "equity") equity_ = value;
    else if (key == "risk_fraction") risk_fraction_ = value;
    else throw std::runtime_error("Unknown parameter for larry_connor: " + key);
}

std::vector<std::pair<std::string, double>>
larry_connor_strategy::get_indicator_values(const std::string& symbol) const
{
    std::vector<std::pair<std::string, double>> vals;
    auto it = states_.find(symbol);
    if (it == states_.end()) return vals;

    const auto& st = it->second;
    if (st.ma.ready())
        vals.emplace_back("ma_" + std::to_string(ma_period_), st.ma.value());
    if (st.entry_low.ready())
        vals.emplace_back("low_" + std::to_string(entry_period_), st.entry_low.min());
    if (st.exit_high.ready())
        vals.emplace_back("high_" + std::to_string(exit_period_), st.exit_high.max());
    if (st.atr.ready())
        vals.emplace_back("atr_" + std::to_string(atr_period_), st.atr.value());
    return vals;
}

void larry_connor_strategy::reset(uint64_t /*seed*/)
{
    // Clear per-symbol indicator + position state so the next MC trial starts fresh.
    states_.clear();
}
