#include "ema_rsi_atr_pullback_strategy.h"
#include "../strategy_registry.h"
#include "../../execution/position_sizing.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

REGISTER_STRATEGY("ema-rsi-atr-pullback", []() {
    return std::make_shared<ema_rsi_atr_pullback_strategy>();
})

ema_rsi_atr_pullback_strategy::ema_rsi_atr_pullback_strategy(
    std::size_t ema_period,
    std::size_t rsi_period,
    std::size_t atr_period,
    double risk_fraction,
    double atr_stop_multiplier,
    double equity,
    double long_rsi_threshold,
    double short_rsi_threshold)
    : ema_period_(ema_period)
    , rsi_period_(rsi_period)
    , atr_period_(atr_period)
    , long_rsi_threshold_(long_rsi_threshold)
    , short_rsi_threshold_(short_rsi_threshold)
    , atr_stop_multiplier_(atr_stop_multiplier)
    , equity_(equity)
    , risk_fraction_(risk_fraction)
    , states_([this]() {
          return SymbolState(ema_period_, rsi_period_, atr_period_);
      })
{
    if (ema_period_ < 2)
        throw std::runtime_error("ema_period must be >= 2");
    if (rsi_period_ < 2)
        throw std::runtime_error("rsi_period must be >= 2");
    if (atr_period_ < 1)
        throw std::runtime_error("atr_period must be >= 1");
    if (long_rsi_threshold_ >= short_rsi_threshold_)
        throw std::runtime_error("long_rsi_threshold must be < short_rsi_threshold");
    if (risk_fraction_ <= 0.0 || risk_fraction_ > 0.05)
        throw std::runtime_error("risk_fraction must be in range (0, 0.05]");
    if (atr_stop_multiplier_ <= 0.0)
        throw std::runtime_error("atr_stop_multiplier must be > 0");
    if (equity_ <= 0.0)
        throw std::runtime_error("equity must be > 0");
}

bool ema_rsi_atr_pullback_strategy::is_valid_bar(const market_event& mkt)
{
    const double o = mkt.get_open();
    const double h = mkt.get_high();
    const double l = mkt.get_low();
    const double c = mkt.get_close();

    if (!std::isfinite(o) || !std::isfinite(h) || !std::isfinite(l) || !std::isfinite(c))
        return false;
    if (o <= 0.0 || h <= 0.0 || l <= 0.0 || c <= 0.0)
        return false;
    if (h < l)
        return false;
    if (h < o || h < c)
        return false;
    if (l > o || l > c)
        return false;
    return true;
}

bool ema_rsi_atr_pullback_strategy::has_active_trade() const
{
    for (std::size_t i = 0; i < states_.size(); ++i)
    {
        const std::string& sym = states_.table().resolve(static_cast<std::uint16_t>(i));
        const auto* st = states_.find(sym);
        if (st && (st->state != trade_state::flat || st->open_qty > 0.0))
            return true;
    }
    return false;
}

void ema_rsi_atr_pullback_strategy::reinit_symbol_states()
{
    states_.clear();
}

std::optional<order_event> ema_rsi_atr_pullback_strategy::on_market(const market_event& mkt)
{
    if (!is_valid_bar(mkt))
        return std::nullopt;

    auto slot = states_.get(mkt.get_symbol());
    auto& st = slot.state;
    const std::string& interned = slot.symbol;

    const double high  = mkt.get_high();
    const double low   = mkt.get_low();
    const double close = mkt.get_close();

    // Update indicators
    st.ema.update(close);
    st.atr.update(high, low, close);
    const std::optional<double> curr_rsi = st.rsi.update(close);

    // If currently in a position, evaluate trend-invalidation exit first
    if (st.state == trade_state::long_open)
    {
        if (close < st.ema.value())
        {
            if (st.open_qty > 0.0 && st.opener_order_id != 0)
            {
                order_event order(mkt.get_timestamp(), interned,
                                  order_type::market, order_side::sell,
                                  st.open_qty, close);
                order.set_opener_order_id(st.opener_order_id);
                st.state = trade_state::exit_pending_long;
                st.prev_rsi = curr_rsi;
                return order;
            }
        }
        st.prev_rsi = curr_rsi;
        return std::nullopt;
    }
    else if (st.state == trade_state::short_open)
    {
        if (close > st.ema.value())
        {
            if (st.open_qty > 0.0 && st.opener_order_id != 0)
            {
                order_event order(mkt.get_timestamp(), interned,
                                  order_type::market, order_side::buy,
                                  st.open_qty, close);
                order.set_opener_order_id(st.opener_order_id);
                st.state = trade_state::exit_pending_short;
                st.prev_rsi = curr_rsi;
                return order;
            }
        }
        st.prev_rsi = curr_rsi;
        return std::nullopt;
    }
    else if (st.state != trade_state::flat)
    {
        // entry_pending or exit_pending -> do not emit another order
        st.prev_rsi = curr_rsi;
        return std::nullopt;
    }

    // From here on, st.state == trade_state::flat.
    // Warm-up check: EMA, RSI, ATR must all be ready, AND previous completed RSI value must exist.
    if (!st.ema.ready() || !st.rsi.ready() || !st.atr.ready() ||
        !st.prev_rsi.has_value() || !curr_rsi.has_value())
    {
        if (curr_rsi.has_value())
            st.prev_rsi = curr_rsi;
        return std::nullopt;
    }

    const double ema_val  = st.ema.value();
    const double rsi_prev = *st.prev_rsi;
    const double rsi_curr = *curr_rsi;
    const double atr_val  = st.atr.value();

    // Pullback trigger & trend filter rules:
    // Long: close > EMA AND rsi_prev <= long_rsi_threshold AND rsi_curr > long_rsi_threshold
    // Short: close < EMA AND rsi_prev >= short_rsi_threshold AND rsi_curr < short_rsi_threshold
    // close == EMA is neutral.
    const bool long_signal  = allow_long_ && (close > ema_val) &&
                             (rsi_prev <= long_rsi_threshold_) &&
                             (rsi_curr > long_rsi_threshold_);

    const bool short_signal = allow_short_ && (close < ema_val) &&
                              (rsi_prev >= short_rsi_threshold_) &&
                              (rsi_curr < short_rsi_threshold_);

    // Consume this bar's RSI observation before testing orderability. A
    // sizing or routing rejection therefore cannot replay this same recross:
    // the engine returns the optimistic entry state to flat through
    // set_position_open(false), and only a later, fresh recross may enter.
    st.prev_rsi = curr_rsi;

    if (!long_signal && !short_signal)
        return std::nullopt;

    // Validate inputs for position sizing
    if (!(equity_ > 0.0) || !std::isfinite(equity_))
        return std::nullopt;
    if (!(atr_val > 0.0) || !std::isfinite(atr_val))
        return std::nullopt;

    const double stop_dist = atr_val * atr_stop_multiplier_;
    if (!(stop_dist > 0.0) || !std::isfinite(stop_dist))
        return std::nullopt;

    const double stop_price = long_signal ? (close - stop_dist) : (close + stop_dist);
    if (!(stop_price > 0.0) || !std::isfinite(stop_price))
        return std::nullopt;

    // Compute fee- and slippage-aware risk quantity
    truetest::risk::risk_size_inputs in;
    in.equity            = equity_;
    in.risk_fraction     = risk_fraction_;
    in.entry_price       = close;
    in.stop_price        = stop_price;
    in.is_long           = long_signal;
    in.entry_fee_rate    = entry_fee_rate_;
    in.exit_fee_rate     = exit_fee_rate_;
    in.entry_slip_bps    = entry_slip_bps_;
    in.exit_slip_bps     = exit_slip_bps_;
    in.fixed_fee_per_leg = fixed_fee_per_leg_;
    in.max_notional_frac = max_notional_frac_;

    const double raw_qty = truetest::risk::compute_risk_quantity(in);
    if (!(raw_qty > 0.0) || !std::isfinite(raw_qty))
        return std::nullopt;

    double final_qty = raw_qty;
    if (quantity_step_ > 0.0)
    {
        final_qty = std::floor(raw_qty / quantity_step_) * quantity_step_;
    }

    if (!(final_qty > 0.0) || !std::isfinite(final_qty))
        return std::nullopt;

    // Create protective ATR stop-loss intent for ExitManager
    truetest::exits::exit_intent ei;
    ei.symbol          = interned;
    ei.close_side      = long_signal ? order_side::sell : order_side::buy;
    ei.qty             = final_qty;
    ei.qty_fraction    = 1.0;
    ei.stop_loss       = stop_price;
    ei.reference_entry = close;
    ei.strategy_name   = "ema-rsi-atr-pullback";

    pending_intents_.push_back(std::move(ei));

    st.state              = long_signal ? trade_state::entry_pending_long : trade_state::entry_pending_short;
    st.expected_entry_qty = final_qty;
    st.open_qty           = 0.0;
    st.opener_order_id    = 0;

    return order_event(mkt.get_timestamp(), interned,
                       order_type::market,
                       long_signal ? order_side::buy : order_side::sell,
                       final_qty, close);
}

void ema_rsi_atr_pullback_strategy::on_fill(const fill_event& fill, std::uint64_t opener_order_id)
{
    if (opener_order_id == 0)
        return;

    auto* st_ptr = states_.find_mut(fill.get_symbol());
    if (!st_ptr)
        return;
    auto& st = *st_ptr;

    const std::uint64_t fill_order_id = fill.get_order_id();
    const double fill_qty = fill.get_filled_quantity();

    // Opener fill: opener_order_id == fill_order_id
    if (opener_order_id == fill_order_id)
    {
        if (st.state == trade_state::entry_pending_long)
        {
            if (st.opener_order_id == 0)
                st.opener_order_id = opener_order_id;

            if (st.opener_order_id == opener_order_id)
            {
                st.open_qty += fill_qty;
                st.state = trade_state::long_open;
            }
        }
        else if (st.state == trade_state::long_open)
        {
            if (st.opener_order_id == opener_order_id)
            {
                st.open_qty += fill_qty;
            }
        }
        else if (st.state == trade_state::entry_pending_short)
        {
            if (st.opener_order_id == 0)
                st.opener_order_id = opener_order_id;

            if (st.opener_order_id == opener_order_id)
            {
                st.open_qty += fill_qty;
                st.state = trade_state::short_open;
            }
        }
        else if (st.state == trade_state::short_open)
        {
            if (st.opener_order_id == opener_order_id)
            {
                st.open_qty += fill_qty;
            }
        }
    }
    // Closer fill: opener_order_id matches our tracked opener
    else if (opener_order_id == st.opener_order_id && st.opener_order_id != 0)
    {
        if (st.open_qty > 0.0)
        {
            st.open_qty -= fill_qty;
            if (st.open_qty <= 1e-9)
            {
                st.open_qty           = 0.0;
                st.opener_order_id    = 0;
                st.expected_entry_qty = 0.0;
                st.state              = trade_state::flat;
            }
        }
    }
}

void ema_rsi_atr_pullback_strategy::set_position_open(const std::string& symbol, bool open)
{
    auto* st_ptr = states_.find_mut(symbol);
    if (!st_ptr)
        return;
    auto& st = *st_ptr;

    if (!open)
    {
        if (st.state == trade_state::entry_pending_long || st.state == trade_state::entry_pending_short)
        {
            st.state              = trade_state::flat;
            st.open_qty           = 0.0;
            st.expected_entry_qty = 0.0;
            st.opener_order_id    = 0;
        }
        else if (st.open_qty <= 1e-9)
        {
            st.state              = trade_state::flat;
            st.open_qty           = 0.0;
            st.expected_entry_qty = 0.0;
            st.opener_order_id    = 0;
        }
    }
    else
    {
        if (st.state == trade_state::exit_pending_long)
            st.state = trade_state::long_open;
        else if (st.state == trade_state::exit_pending_short)
            st.state = trade_state::short_open;
    }
}

std::vector<truetest::exits::exit_intent>
ema_rsi_atr_pullback_strategy::take_pending_exit_intents()
{
    auto out = std::move(pending_intents_);
    pending_intents_.clear();
    return out;
}

std::vector<param_def> ema_rsi_atr_pullback_strategy::get_param_schema() const
{
    return {
        {"ema_period", static_cast<double>(ema_period_), 2.0, 10000.0, "EMA trend filter period"},
        {"rsi_period", static_cast<double>(rsi_period_), 2.0, 1000.0, "RSI pull-back period"},
        {"atr_period", static_cast<double>(atr_period_), 1.0, 1000.0, "ATR volatility period"},
        {"long_rsi_threshold", long_rsi_threshold_, 0.0, 100.0, "RSI pullback threshold for long entry"},
        {"short_rsi_threshold", short_rsi_threshold_, 0.0, 100.0, "RSI pullback threshold for short entry"},
        {"atr_stop_multiplier", atr_stop_multiplier_, 0.01, 100.0, "ATR multiplier for initial stop loss"},
        {"equity", equity_, 1.0, 1e18, "Account equity for position sizing"},
        {"risk_fraction", risk_fraction_, 0.0001, 0.05, "Fraction of equity risked per trade"},
        {"allow_long", allow_long_ ? 1.0 : 0.0, 0.0, 1.0, "Allow long entries (1=yes, 0=no)"},
        {"allow_short", allow_short_ ? 1.0 : 0.0, 0.0, 1.0, "Allow short entries (1=yes, 0=no)"},
        {"entry_fee_rate", entry_fee_rate_, 0.0, 0.05, "Entry fee rate as fraction of notional"},
        {"exit_fee_rate", exit_fee_rate_, 0.0, 0.05, "Exit fee rate as fraction of notional"},
        {"entry_slip_bps", entry_slip_bps_, 0.0, 500.0, "Adverse entry slippage (bps)"},
        {"exit_slip_bps", exit_slip_bps_, 0.0, 500.0, "Adverse exit slippage (bps)"},
        {"fixed_fee_per_leg", fixed_fee_per_leg_, 0.0, 1e6, "Fixed fee per leg"},
        {"max_notional_frac", max_notional_frac_, 0.0, 10.0, "Max position notional as fraction of equity (0=off)"},
        {"quantity_step", quantity_step_, 0.0, 1e6, "Quantity rounding step (0=off)"}
    };
}

void ema_rsi_atr_pullback_strategy::set_param(const std::string& key, double value)
{
    if (!std::isfinite(value))
        throw std::runtime_error("Parameter value must be finite: " + key);

    if (key == "ema_period")
    {
        if (std::floor(value) != value || value < 2.0)
            throw std::runtime_error("ema_period must be an integer >= 2");
        if (has_active_trade())
            throw std::runtime_error("Cannot change ema_period during an active trade");
        ema_period_ = static_cast<std::size_t>(value);
        reinit_symbol_states();
    }
    else if (key == "rsi_period")
    {
        if (std::floor(value) != value || value < 2.0)
            throw std::runtime_error("rsi_period must be an integer >= 2");
        if (has_active_trade())
            throw std::runtime_error("Cannot change rsi_period during an active trade");
        rsi_period_ = static_cast<std::size_t>(value);
        reinit_symbol_states();
    }
    else if (key == "atr_period")
    {
        if (std::floor(value) != value || value < 1.0)
            throw std::runtime_error("atr_period must be an integer >= 1");
        if (has_active_trade())
            throw std::runtime_error("Cannot change atr_period during an active trade");
        atr_period_ = static_cast<std::size_t>(value);
        reinit_symbol_states();
    }
    else if (key == "long_rsi_threshold")
    {
        if (value < 0.0 || value >= 100.0)
            throw std::runtime_error("long_rsi_threshold must be in range [0, 100)");
        if (value >= short_rsi_threshold_)
            throw std::runtime_error("long_rsi_threshold must be < short_rsi_threshold");
        long_rsi_threshold_ = value;
    }
    else if (key == "short_rsi_threshold")
    {
        if (value <= 0.0 || value > 100.0)
            throw std::runtime_error("short_rsi_threshold must be in range (0, 100]");
        if (value <= long_rsi_threshold_)
            throw std::runtime_error("short_rsi_threshold must be > long_rsi_threshold");
        short_rsi_threshold_ = value;
    }
    else if (key == "atr_stop_multiplier")
    {
        if (!(value > 0.0))
            throw std::runtime_error("atr_stop_multiplier must be strictly positive");
        atr_stop_multiplier_ = value;
    }
    else if (key == "equity")
    {
        if (!(value > 0.0))
            throw std::runtime_error("equity must be strictly positive");
        equity_ = value;
    }
    else if (key == "risk_fraction")
    {
        if (!(value > 0.0) || value > 0.05)
            throw std::runtime_error("risk_fraction must be in range (0, 0.05]");
        risk_fraction_ = value;
    }
    else if (key == "allow_long")
    {
        if (value != 0.0 && value != 1.0)
            throw std::runtime_error("allow_long must be exactly 0.0 or 1.0");
        allow_long_ = (value == 1.0);
    }
    else if (key == "allow_short")
    {
        if (value != 0.0 && value != 1.0)
            throw std::runtime_error("allow_short must be exactly 0.0 or 1.0");
        allow_short_ = (value == 1.0);
    }
    else if (key == "entry_fee_rate")
    {
        if (value < 0.0)
            throw std::runtime_error("entry_fee_rate must be non-negative");
        entry_fee_rate_ = value;
    }
    else if (key == "exit_fee_rate")
    {
        if (value < 0.0)
            throw std::runtime_error("exit_fee_rate must be non-negative");
        exit_fee_rate_ = value;
    }
    else if (key == "entry_slip_bps")
    {
        if (value < 0.0)
            throw std::runtime_error("entry_slip_bps must be non-negative");
        entry_slip_bps_ = value;
    }
    else if (key == "exit_slip_bps")
    {
        if (value < 0.0)
            throw std::runtime_error("exit_slip_bps must be non-negative");
        exit_slip_bps_ = value;
    }
    else if (key == "fixed_fee_per_leg")
    {
        if (value < 0.0)
            throw std::runtime_error("fixed_fee_per_leg must be non-negative");
        fixed_fee_per_leg_ = value;
    }
    else if (key == "max_notional_frac")
    {
        if (value < 0.0)
            throw std::runtime_error("max_notional_frac must be non-negative");
        max_notional_frac_ = value;
    }
    else if (key == "quantity_step")
    {
        if (value < 0.0)
            throw std::runtime_error("quantity_step must be non-negative");
        quantity_step_ = value;
    }
    else
    {
        throw std::runtime_error("Unknown parameter: " + key);
    }
}

std::vector<std::pair<std::string, double>>
ema_rsi_atr_pullback_strategy::get_indicator_values(const std::string& symbol) const
{
    const auto* st = states_.find(symbol);
    if (!st)
        return {};

    std::vector<std::pair<std::string, double>> vals;
    if (st->ema.ready())
        vals.emplace_back("ema_" + std::to_string(ema_period_), st->ema.value());
    if (st->rsi.ready())
        vals.emplace_back("rsi_" + std::to_string(rsi_period_), st->rsi.value());
    if (st->atr.ready())
        vals.emplace_back("atr_" + std::to_string(atr_period_), st->atr.value());

    vals.emplace_back("trade_state", static_cast<double>(static_cast<int>(st->state)));
    vals.emplace_back("open_qty", st->open_qty);

    return vals;
}

void ema_rsi_atr_pullback_strategy::reset(uint64_t /*seed*/)
{
    states_.clear();
    pending_intents_.clear();
}

ema_rsi_atr_pullback_strategy::trade_state
ema_rsi_atr_pullback_strategy::get_trade_state(const std::string& symbol) const
{
    const auto* st = states_.find(symbol);
    if (!st)
        return trade_state::flat;
    return st->state;
}

double ema_rsi_atr_pullback_strategy::get_open_qty(const std::string& symbol) const
{
    const auto* st = states_.find(symbol);
    if (!st)
        return 0.0;
    return st->open_qty;
}

std::uint64_t ema_rsi_atr_pullback_strategy::get_opener_order_id(const std::string& symbol) const
{
    const auto* st = states_.find(symbol);
    if (!st)
        return 0;
    return st->opener_order_id;
}
