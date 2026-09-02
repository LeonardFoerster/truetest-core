#include "mean_reversion_strategy.h"
#include "../strategy_registry.h"
#include "../../core/event.h"
#include "../../execution/position_sizing.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

REGISTER_STRATEGY("mean-reversion", []() {
    return std::make_shared<mean_reversion_strategy>();
})

mean_reversion_strategy::mean_reversion_strategy(std::size_t period, double equity,
                                                   double risk_fraction, double sl_pct, double tp_pct,
                                                   std::size_t atr_period)
    : period_(period), equity_(equity), risk_fraction_(risk_fraction),
      sl_pct_(sl_pct), tp_pct_(tp_pct), atr_period_(atr_period)
    , states_([this]() {
          return symbol_state(period_, atr_period_, swing_strength_, swing_history_);
      })
{}

// Fixed-risk sizing: risk_fraction is the stop-loss budget as a fraction of
// equity. Fees and adverse slip are folded into per-unit risk so the budget
// is not silently overshot. max_notional_frac (if set) is an independent cap.
double mean_reversion_strategy::compute_quantity_with_sl(double entry, double sl_price,
                                                         bool is_long) const
{
    truetest::risk::risk_size_inputs in;
    in.equity            = equity_;
    in.risk_fraction     = risk_fraction_;
    in.entry_price       = entry;
    in.stop_price        = sl_price;
    in.is_long           = is_long;
    in.entry_fee_rate    = entry_fee_rate_;
    in.exit_fee_rate     = exit_fee_rate_;
    in.entry_slip_bps    = entry_slip_bps_;
    in.exit_slip_bps     = exit_slip_bps_;
    in.fixed_fee_per_leg = fixed_fee_per_leg_;
    in.max_notional_frac = max_notional_frac_;
    return truetest::risk::compute_risk_quantity(in);
}

void mean_reversion_strategy::update_indicators(symbol_state& st,
                                                double high, double low, double close)
{
    // SMA always (entry edge detection). ATR/swing only when exit_style needs them:
    //   pct → SMA only
    //   atr → SMA + ATR
    //   fib → SMA + ATR + swing
    // This is the main per-bar cost cut for non-fib styles and avoids deque work
    // in swing_detector when structure is unused.
    if (needs_atr())
        (void)st.atr.update(high, low, close);
    if (needs_swing())
        st.swing.update(high, low, close);
}

mean_reversion_strategy::sma_side
mean_reversion_strategy::side_of(double price, double sma_value)
{
    if (price < sma_value) return sma_side::below;
    if (price > sma_value) return sma_side::above;
    return sma_side::equal;
}

// Edge-triggered mean-reversion entry:
//   - Long  only on transition into price < SMA (not every bar while below)
//   - Short only on transition into price > SMA
// Optimistic gate lock on emit stops free-fire between signal and fill.
std::optional<order_event>
mean_reversion_strategy::try_entry(symbol_state& st,
                                   const std::string& symbol,
                                   std::chrono::system_clock::time_point ts,
                                   double price, double sma_value)
{
    if (st.gate.position_open)
        return std::nullopt;

    const sma_side side = side_of(price, sma_value);
    const sma_side prev = st.gate.prev_side;
    st.gate.prev_side = side;

    if (side == sma_side::equal)
        return std::nullopt;

    const bool long_edge  = (side == sma_side::below) && (prev != sma_side::below);
    const bool short_edge = (side == sma_side::above) && (prev != sma_side::above);
    if (!long_edge && !short_edge)
        return std::nullopt;

    const bool is_long = long_edge;
    const double entry = price;
    const double tentative_sl = compute_intended_sl(st, entry, is_long);
    const double qty = compute_quantity_with_sl(entry, tentative_sl, is_long);
    if (qty <= 0.0)
        return std::nullopt;

    auto intents = create_exit_intents(st, symbol, entry, qty, is_long);
    if (pending_intents_.capacity() < pending_intents_.size() + intents.size())
        pending_intents_.reserve(pending_intents_.size() + intents.size());
    pending_intents_.insert(pending_intents_.end(),
                            std::make_move_iterator(intents.begin()),
                            std::make_move_iterator(intents.end()));

    st.gate.position_open = true;

    return order_event(ts, symbol, order_type::market,
                       is_long ? order_side::buy : order_side::sell,
                       qty, entry);
}

// Exits are owned by the engine's ExitManager via the bracket registered
// at entry - there is intentionally no signal-based SELL here.
std::optional<order_event> mean_reversion_strategy::on_market(const market_event& mkt)
{
    // One intern_id hash + dense slot; interned symbol used for any order/intent.
    auto slot = states_.get(mkt.get_symbol());
    auto& st = slot.state;
    auto sma_value = st.sma.update(mkt.get_close());
    if (!sma_value) return std::nullopt;

    // Heavy indicators: keep warm every bar so SL/structure stay correct on the
    // next edge. Style-gated so pct mode never pays for ATR/swing deques.
    update_indicators(st, mkt.get_high(), mkt.get_low(), mkt.get_close());

    return try_entry(st, slot.symbol, mkt.get_timestamp(),
                     mkt.get_close(), *sma_value);
}

std::optional<order_event> mean_reversion_strategy::on_tick(const tick_event& te)
{
    auto slot = states_.get(te.get_symbol());
    auto& st = slot.state;
    auto sma_value = st.sma.update(te.get_price());
    if (!sma_value) return std::nullopt;

    const double price = te.get_price();
    update_indicators(st, price, price, price);

    return try_entry(st, slot.symbol, te.get_timestamp(), price, *sma_value);
}

std::vector<truetest::exits::exit_intent>
mean_reversion_strategy::take_pending_exit_intents()
{
    auto out = std::move(pending_intents_);
    pending_intents_.clear();
    return out;
}

void mean_reversion_strategy::set_position_open(const std::string& symbol, bool open)
{
    states_.get(symbol).state.gate.position_open = open;
    // On flat: keep prev_side so re-entry requires a fresh SMA cross.
}

std::vector<std::pair<std::string, double>>
mean_reversion_strategy::get_indicator_values(const std::string& symbol) const
{
    std::vector<std::pair<std::string, double>> vals;

    const auto* st = states_.find(symbol);
    if (!st) return vals;

    if (st->sma.ready())
        vals.emplace_back("sma_" + std::to_string(period_), st->sma.value());
    if (needs_atr() && st->atr.ready())
        vals.emplace_back("atr_" + std::to_string(atr_period_), st->atr.value());
    if (needs_swing() && st->swing.ready())
        vals.emplace_back("swing_phase",
                          static_cast<double>(static_cast<int>(st->swing.phase())));

    return vals;
}

double mean_reversion_strategy::compute_intended_sl(symbol_state& st, double entry,
                                                    bool is_long) const
{
    const bool use_fib = (exit_style_ == "fib");
    const bool use_atr = (exit_style_ == "atr");

    if (use_fib && st.swing.ready() && st.atr.ready())
    {
        auto opposing = is_long ? st.swing.last_confirmed_swing_high()
                                : st.swing.last_confirmed_swing_low();

        double impulse_high = is_long ? (opposing ? opposing->price : entry * 1.03) : entry;
        double impulse_low  = is_long ? entry : (opposing ? opposing->price : entry * 0.97);

        double impulse_range = std::abs(impulse_high - impulse_low);
        const double atrv = st.atr.value();
        if (impulse_range < 1.2 * atrv)
        {
            return is_long ? entry - (sl_atr_mult_ * atrv)
                           : entry + (sl_atr_mult_ * atrv);
        }

        if (impulse_range < 0.001 * entry)
        {
            impulse_high = is_long ? entry * 1.02 : entry;
            impulse_low  = is_long ? entry : entry * 0.98;
        }

        double min_dist = std::max(0.4 * atrv, 0.003 * entry);

        if (is_long)
        {
            double sl = entry - (sl_atr_mult_ * atrv);
            if (opposing) sl = std::min(sl, opposing->price - 0.2 * atrv);
            if (entry - sl < min_dist) sl = entry - min_dist;
            return sl;
        }

        double sl = entry + (sl_atr_mult_ * atrv);
        if (opposing) sl = std::max(sl, opposing->price + 0.2 * atrv);
        if (sl - entry < min_dist) sl = entry + min_dist;
        return sl;
    }

    if ((use_atr || use_fib) && st.atr.ready() && st.atr.value() > 0.0)
    {
        return is_long ? entry - (sl_atr_mult_ * st.atr.value())
                       : entry + (sl_atr_mult_ * st.atr.value());
    }

    return is_long ? entry * (1.0 - sl_pct_)
                   : entry * (1.0 + sl_pct_);
}

std::vector<truetest::exits::exit_intent>
mean_reversion_strategy::create_exit_intents(symbol_state& st,
                                             const std::string& symbol,
                                             double entry,
                                             double qty,
                                             bool is_long)
{
    using namespace truetest::exits;

    std::vector<exit_intent> result;

    auto base = [this, &st, &symbol, entry, qty, is_long]() -> std::optional<exit_intent> {
        const bool want_fib = (exit_style_ == "fib");

        if (want_fib && st.swing.ready() && st.atr.ready())
        {
            auto opposing = is_long ? st.swing.last_confirmed_swing_high()
                                    : st.swing.last_confirmed_swing_low();

            double impulse_high = is_long ? (opposing ? opposing->price : entry * 1.03) : entry;
            double impulse_low  = is_long ? entry : (opposing ? opposing->price : entry * 0.97);

            if (std::abs(impulse_high - impulse_low) < 0.001 * entry)
            {
                impulse_high = is_long ? entry * 1.02 : entry;
                impulse_low  = is_long ? entry : entry * 0.98;
            }

            const double atrv = st.atr.value();
            double min_dist   = std::max(0.4 * atrv, 0.003 * entry);

            exit_intent ei;
            ei.symbol          = symbol;
            ei.close_side      = is_long ? order_side::sell : order_side::buy;
            ei.qty             = qty;
            ei.reference_entry = entry;
            ei.strategy_name   = "mean-reversion";

            if (is_long)
            {
                double sl = entry - (sl_atr_mult_ * atrv);
                if (opposing) sl = std::min(sl, opposing->price - 0.2 * atrv);
                if (entry - sl < min_dist) sl = entry - min_dist;

                double tp = entry + (fib_tp_extension_ * std::abs(impulse_high - impulse_low));
                if (tp - entry < 1.5 * atrv) tp = entry + 1.5 * atrv;

                ei.stop_loss   = sl;
                ei.take_profit = tp;
            }
            else
            {
                double sl = entry + (sl_atr_mult_ * atrv);
                if (opposing) sl = std::max(sl, opposing->price + 0.2 * atrv);
                if (sl - entry < min_dist) sl = entry + min_dist;

                double tp = entry - (fib_tp_extension_ * std::abs(impulse_high - impulse_low));
                if (entry - tp < 1.5 * atrv) tp = entry - 1.5 * atrv;

                ei.stop_loss   = sl;
                ei.take_profit = tp;
            }
            return ei;
        }

        if (st.atr.ready() && st.atr.value() > 0.0 && needs_atr())
        {
            exit_intent ei;
            ei.symbol          = symbol;
            ei.close_side      = is_long ? order_side::sell : order_side::buy;
            ei.qty             = qty;
            ei.reference_entry = entry;
            ei.strategy_name   = "mean-reversion";

            const double atrv = st.atr.value();
            if (is_long)
            {
                ei.stop_loss   = entry - (sl_atr_mult_ * atrv);
                ei.take_profit = entry + (tp_atr_mult_ * atrv);
            }
            else
            {
                ei.stop_loss   = entry + (sl_atr_mult_ * atrv);
                ei.take_profit = entry - (tp_atr_mult_ * atrv);
            }
            return ei;
        }

        return is_long
            ? make_long_exit_intent(symbol, entry, qty, sl_pct_, tp_pct_, "mean-reversion")
            : make_short_exit_intent(symbol, entry, qty, sl_pct_, tp_pct_, "mean-reversion");
    }();

    if (!base) return result;

    bool do_scale = (exit_style_ == "fib" || exit_style_ == "atr") &&
                    scale_out_ratio_ > 0.0 && scale_out_ratio_ < 1.0;

    if (do_scale)
    {
        exit_intent first = *base;
        first.qty_fraction = scale_out_ratio_;
        result.push_back(std::move(first));

        exit_intent runner = *base;
        runner.qty_fraction = 1.0 - scale_out_ratio_;
        runner.take_profit.reset();

        double atrv = (needs_atr() && st.atr.ready()) ? st.atr.value() : 0.0;
        if (atrv > 0.0)
        {
            double trail_pct = (trail_atr_mult_ * atrv) / entry;
            runner.trailing_pct = std::clamp(trail_pct, 0.003, 0.08);
        }
        else
        {
            runner.trailing_pct = std::clamp(trail_atr_mult_ * 0.01, 0.003, 0.05);
        }
        result.push_back(std::move(runner));
    }
    else
    {
        result.push_back(std::move(*base));
    }

    return result;
}

std::vector<param_def> mean_reversion_strategy::get_param_schema() const
{
    return {
        {"period", static_cast<double>(period_), 1, 10000, "SMA lookback period"},
        {"equity", equity_, 0, 1e18, "Account equity for position sizing"},
        {"risk_fraction", risk_fraction_, 0, 1, "Stop-risk budget as fraction of equity"},
        {"sl_pct", sl_pct_, 0, 1, "Stop loss % (legacy)"},
        {"tp_pct", tp_pct_, 0, 1, "Take profit % (legacy)"},
        {"entry_fee_rate", entry_fee_rate_, 0, 0.05, "Entry fee as fraction of notional"},
        {"exit_fee_rate", exit_fee_rate_, 0, 0.05, "Exit fee as fraction of notional"},
        {"entry_slip_bps", entry_slip_bps_, 0, 500, "Adverse entry slippage (bps)"},
        {"exit_slip_bps", exit_slip_bps_, 0, 500, "Adverse exit/stop slippage (bps)"},
        {"fixed_fee_per_leg", fixed_fee_per_leg_, 0, 1e6, "Fixed fee per fill leg"},
        {"max_notional_frac", max_notional_frac_, 0, 10, "Optional max position notional / equity (0=off)"},
        {"atr_period", static_cast<double>(atr_period_), 5, 100, "ATR period"},
        {"sl_atr_mult", sl_atr_mult_, 0.1, 10.0, "SL in ATR units"},
        {"tp_atr_mult", tp_atr_mult_, 0.1, 20.0, "TP as R-multiple"},
        {"swing_strength", static_cast<double>(swing_strength_), 1, 5, "Swing strength"},
        {"fib_sl_retracement", fib_sl_retracement_, 0.1, 0.9, "Fib SL level"},
        {"fib_tp_extension", fib_tp_extension_, 1.0, 3.0, "Fib TP extension"},
        {"atr_buffer_mult_sl", atr_buffer_mult_sl_, 0.0, 1.0, "ATR buffer SL"},
        {"atr_buffer_mult_tp", atr_buffer_mult_tp_, 0.0, 1.0, "ATR buffer TP"},
        {"exit_style", exit_style_ == "pct" ? 0.0
                         : exit_style_ == "atr" ? 1.0 : 2.0,
         0.0, 2.0, "0=pct | 1=atr | 2=fib"},
        {"scale_out_ratio", scale_out_ratio_, 0.0, 1.0, "Fraction at first TP"},
        {"trail_atr_mult", trail_atr_mult_, 0.5, 5.0, "Trailing ATR mult for runner"},
    };
}

void mean_reversion_strategy::set_param(const std::string& key, double value)
{
    if (key == "period") { period_ = static_cast<std::size_t>(value); states_.clear(); }
    else if (key == "equity") equity_ = value;
    else if (key == "risk_fraction") risk_fraction_ = value;
    else if (key == "sl_pct") sl_pct_ = value;
    else if (key == "tp_pct") tp_pct_ = value;
    else if (key == "entry_fee_rate") entry_fee_rate_ = value;
    else if (key == "exit_fee_rate") exit_fee_rate_ = value;
    else if (key == "entry_slip_bps") entry_slip_bps_ = value;
    else if (key == "exit_slip_bps") exit_slip_bps_ = value;
    else if (key == "fixed_fee_per_leg") fixed_fee_per_leg_ = value;
    else if (key == "max_notional_frac") max_notional_frac_ = value;
    else if (key == "atr_period") { atr_period_ = static_cast<std::size_t>(value); states_.clear(); }
    else if (key == "sl_atr_mult") sl_atr_mult_ = value;
    else if (key == "tp_atr_mult") tp_atr_mult_ = value;
    else if (key == "swing_strength") { swing_strength_ = static_cast<std::size_t>(value); states_.clear(); }
    else if (key == "fib_sl_retracement") fib_sl_retracement_ = value;
    else if (key == "fib_tp_extension") fib_tp_extension_ = value;
    else if (key == "atr_buffer_mult_sl") atr_buffer_mult_sl_ = value;
    else if (key == "atr_buffer_mult_tp") atr_buffer_mult_tp_ = value;
    else if (key == "exit_style") {
        if (value == 0.0) exit_style_ = "pct";
        else if (value == 1.0) exit_style_ = "atr";
        else exit_style_ = "fib";
        states_.clear();
    }
    else if (key == "scale_out_ratio") scale_out_ratio_ = value;
    else if (key == "trail_atr_mult") trail_atr_mult_ = value;
    else throw std::runtime_error("Unknown parameter: " + key);
}

void mean_reversion_strategy::reset(uint64_t /*seed*/)
{
    states_.clear();
    pending_intents_.clear();
}
