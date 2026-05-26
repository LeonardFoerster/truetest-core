#include "structure_continuation_strategy.h"
#include "strategy_registry.h"

#include <algorithm>
#include <cmath>

REGISTER_STRATEGY("structure-continuation", []() {
    return std::make_shared<structure_continuation_strategy>();
})

namespace {
    constexpr std::size_t MAX_HISTORY = 64;
}

structure_continuation_strategy::structure_continuation_strategy()
    : structure_continuation_strategy(0.01, 2, 32)
{
}

structure_continuation_strategy::structure_continuation_strategy(
        double risk_fraction,
        std::size_t swing_strength,
        std::size_t swing_history)
    : risk_fraction_(risk_fraction)
    , swing_strength_(swing_strength)
    , swing_history_(swing_history)
{
}

structure_continuation_strategy::SymbolState&
structure_continuation_strategy::get_state(const std::string& symbol)
{
    auto it = states_.find(symbol);
    if (it == states_.end())
    {
        SymbolState st;
        st.swing = swing_detector(swing_strength_, swing_history_);
        auto [ins, _] = states_.emplace(symbol, std::move(st));
        return ins->second;
    }
    return it->second;
}

void structure_continuation_strategy::update_all_indicators(
    SymbolState& st,
    double /*open*/, double high, double low, double close)
{
    // Order is important
    auto atr_opt = st.atr.update(high, low, close);
    (void)st.ema50.update(close);
    (void)st.ema100.update(close);
    (void)st.stoch.update(high, low, close);
    st.swing.update(high, low, close);

    if (st.ema50.ready() && st.ema100.ready() && st.stoch.ready() && st.swing.ready() && st.atr.ready())
    {
        st.regime.update(st.ema50.value(), st.ema100.value(), st.swing, st.atr);
    }

    st.last_close = close;
}

bool structure_continuation_strategy::is_valid_long_setup(const SymbolState& st) const
{
    if (!st.ema50.ready() || !st.ema100.ready() || !st.stoch.ready() || !st.swing.ready())
        return false;

    const bool structure_ok = (st.swing.phase() == structure_phase::uptrend) ||
                              (st.swing.last_higher_high_price().has_value());

    const bool ema_bias = st.last_close > st.ema100.value();

    const bool stoch_ok = st.stoch.k() > st.stoch.d() &&
                          (st.stoch.k() > 20.0 || st.stoch.d() > 20.0); // avoid extreme oversold cross

    const bool regime_ok = !st.regime.is_sideways() &&
                           !st.regime.is_wide();

    return structure_ok && ema_bias && stoch_ok && regime_ok;
}

bool structure_continuation_strategy::is_valid_short_setup(const SymbolState& st) const
{
    if (!st.ema50.ready() || !st.ema100.ready() || !st.stoch.ready() || !st.swing.ready())
        return false;

    const bool structure_ok = (st.swing.phase() == structure_phase::downtrend) ||
                              (st.swing.last_lower_low_price().has_value());

    const bool ema_bias = st.last_close < st.ema100.value();

    const bool stoch_ok = st.stoch.k() < st.stoch.d() &&
                          (st.stoch.k() < 80.0 || st.stoch.d() < 80.0);

    const bool regime_ok = !st.regime.is_sideways() &&
                           !st.regime.is_wide();

    return structure_ok && ema_bias && stoch_ok && regime_ok;
}

void structure_continuation_strategy::advance_continuation_fsm(
    SymbolState& st,
    bool long_signal,
    bool short_signal)
{
    using Phase = SymbolState::ContinuationPhase;

    switch (st.phase)
    {
    case Phase::NORMAL:
        if (st.regime.is_sideways())
        {
            st.phase = Phase::AFTER_SIDEWAYS;
            st.bars_since_sideways_exit = 0;
            st.orientation_bias = 0;
            st.signals_since_orientation = 0;
        }
        break;

    case Phase::AFTER_SIDEWAYS:
        if (!st.regime.is_sideways())
        {
            st.bars_since_sideways_exit++;
        }

        if (long_signal || short_signal)
        {
            // First signal after sideways → orientation only
            st.phase = Phase::ORIENTATION_PENDING;
            st.orientation_bias = long_signal ? 1 : -1;
            st.signals_since_orientation = 1;
        }
        break;

    case Phase::ORIENTATION_PENDING:
        if (long_signal || short_signal)
        {
            st.signals_since_orientation++;

            // Second signal in the same direction → ready to trade
            if (st.signals_since_orientation >= 2)
            {
                if ((st.orientation_bias > 0 && long_signal) ||
                    (st.orientation_bias < 0 && short_signal))
                {
                    st.phase = Phase::READY_FOR_CONTINUATION;
                }
            }
        }
        break;

    case Phase::READY_FOR_CONTINUATION:
        // Strategy will consume this state in on_market to emit an order
        break;

    case Phase::IN_TRADE:
    case Phase::COOLDOWN:
        break;
    }

    // Basic timeout / reset logic
    if (st.phase == Phase::AFTER_SIDEWAYS && st.bars_since_sideways_exit > 30)
    {
        st.phase = Phase::NORMAL;
    }
}

double structure_continuation_strategy::compute_quantity(
    double price, double sl_distance, double /*equity*/) const
{
    // TEMPORARY sizing — clearly marked for later replacement by proper risk layer.
    // Current behavior: use risk_fraction_ of a nominal 10k equity for backtest convenience.
    // TODO: Replace with real risk-layer sizing that receives suggested SL from the strategy.
    if (sl_distance <= 0.0) return 0.0;

    const double nominal_equity = 10000.0; // temporary
    const double risk_amount = nominal_equity * risk_fraction_;
    return risk_amount / sl_distance;
}

void structure_continuation_strategy::create_exit_intents(
    const std::string& symbol,
    SymbolState& st,
    double entry_price,
    double qty,
    bool is_long)
{
    if (!st.ema100.ready() || !st.swing.ready() || !st.atr.ready())
        return;

    const double ema100 = st.ema100.value();
    const double atr    = st.atr.value();

    // SL: EMA(100) first, then more conservative of that or previous swing (per user rules + agent synthesis)
    auto low_sp  = st.swing.last_confirmed_swing_low();
    auto high_sp = st.swing.last_confirmed_swing_high();

    double prev_swing = is_long
        ? (low_sp  ? low_sp->price  : 0.0)
        : (high_sp ? high_sp->price : 0.0);

    double sl;
    if (is_long)
    {
        double swing_based = (prev_swing > 0.0) ? prev_swing : entry_price * 0.98;
        sl = std::min(ema100, swing_based);
        double buffer = std::max(0.002 * entry_price, 0.15 * atr);
        sl -= buffer;
    }
    else
    {
        double swing_based = (prev_swing > 0.0) ? prev_swing : entry_price * 1.02;
        sl = std::max(ema100, swing_based);
        double buffer = std::max(0.002 * entry_price, 0.15 * atr);
        sl += buffer;
    }

    // TP: Previous opposing swing extreme, simple "je nach Range" heuristic
    double target_swing = is_long
        ? st.swing.last_higher_high_price().value_or(entry_price * 1.04)
        : st.swing.last_lower_low_price().value_or(entry_price * 0.96);

    double range_proxy = std::abs(target_swing - entry_price);
    bool large_range = (atr > 0.0 && range_proxy > 2.5 * atr);

    double tp = target_swing;
    if (!large_range)
    {
        double conservative = entry_price + 0.85 * (target_swing - entry_price);
        tp = conservative;
    }

    // Scale-out pattern (45% TP1 + 55% runner with trailing, tighter for large-range longs)
    truetest::exits::exit_intent tp_part;
    tp_part.symbol        = symbol;
    tp_part.close_side    = is_long ? order_side::sell : order_side::buy;
    tp_part.qty           = qty;
    tp_part.qty_fraction  = 0.45;
    tp_part.stop_loss     = sl;
    tp_part.take_profit   = tp;
    tp_part.strategy_name = "structure-continuation";

    truetest::exits::exit_intent runner;
    runner.symbol        = symbol;
    runner.close_side    = tp_part.close_side;
    runner.qty           = qty;
    runner.qty_fraction  = 0.55;
    runner.stop_loss     = sl;
    double trail = (is_long && large_range) ? 0.008 : 0.012;
    runner.trailing_pct  = std::clamp(trail, 0.004, 0.025);
    runner.strategy_name = "structure-continuation";

    // Push to strategy-level pending (drained by take_pending_exit_intents)
    pending_intents_.push_back(std::move(tp_part));
    pending_intents_.push_back(std::move(runner));
}

std::optional<order_event> structure_continuation_strategy::on_market(const market_event& mkt)
{
    const std::string& sym = mkt.get_symbol();
    double o = mkt.get_open();
    double h = mkt.get_high();
    double l = mkt.get_low();
    double c = mkt.get_close();

    auto& st = get_state(sym);

    update_all_indicators(st, o, h, l, c);

    bool long_sig  = is_valid_long_setup(st);
    bool short_sig = is_valid_short_setup(st);

    advance_continuation_fsm(st, long_sig, short_sig);

    // Consume READY_FOR_CONTINUATION state
    if (st.phase == SymbolState::ContinuationPhase::READY_FOR_CONTINUATION &&
        !st.position_open)
    {
        bool go_long = (st.orientation_bias > 0) && long_sig;
        bool go_short = (st.orientation_bias < 0) && short_sig;

        if (go_long || go_short)
        {
            // Temporary SL distance for sizing
            double sl_dist = go_long
                ? (c - std::min(st.ema100.value(), c * 0.98))
                : (std::max(st.ema100.value(), c * 1.02) - c);

            double qty = compute_quantity(c, std::max(sl_dist, c * 0.005), 10000.0);
            if (qty <= 0.0) qty = 1.0; // fallback for backtests

            order_side side = go_long ? order_side::buy : order_side::sell;

            // Mark as consuming the state
            st.phase = SymbolState::ContinuationPhase::IN_TRADE;

            // Real exit intents per user's rules (A1)
            // SL = min(EMA100, previous swing) + buffer (conservative)
            // TP = previous opposing swing, with range heuristic for wig vs close
            // Scale-out + trailing; no BE at entry
            create_exit_intents(sym, st, c, qty, go_long);

            return order_event(mkt.get_timestamp(), sym,
                               order_type::market, side, qty, c);
        }
    }

    return std::nullopt;
}

void structure_continuation_strategy::set_position_open(const std::string& symbol, bool open)
{
    auto& st = get_state(symbol);
    st.position_open = open;
    if (!open)
    {
        st.open_lots = 0;
        // Return to scanning after exit
        st.phase = SymbolState::ContinuationPhase::COOLDOWN;
    }
}

std::vector<truetest::exits::exit_intent>
structure_continuation_strategy::take_pending_exit_intents()
{
    // Drain strategy-level pending intents (populated by create_exit_intents at entry)
    auto out = std::move(pending_intents_);
    pending_intents_.clear();
    return out;
}

void structure_continuation_strategy::on_fill(const fill_event& fill, std::uint64_t opener_order_id)
{
    auto& st = get_state(fill.get_symbol());

    if (opener_order_id != 0)
    {
        st.last_opener_id = opener_order_id;
        st.open_lots = 1; // simplified
    }
}

std::vector<param_def> structure_continuation_strategy::get_param_schema() const
{
    return {
        {"risk_fraction", risk_fraction_, 0.001, 0.05, "Temporary risk per trade (will be replaced)"},
        {"swing_strength", static_cast<double>(swing_strength_), 1.0, 5.0, "Pivot strength (bars each side)"},
    };
}

void structure_continuation_strategy::set_param(const std::string& key, double value)
{
    if (key == "risk_fraction") risk_fraction_ = value;
    else if (key == "swing_strength") swing_strength_ = static_cast<std::size_t>(value);
    else throw std::runtime_error("Unknown parameter: " + key);
}

std::vector<std::pair<std::string, double>>
structure_continuation_strategy::get_indicator_values(const std::string& symbol) const
{
    auto it = states_.find(symbol);
    if (it == states_.end()) return {};

    const auto& st = it->second;
    std::vector<std::pair<std::string, double>> vals;

    if (st.ema50.ready())  vals.emplace_back("ema50", st.ema50.value());
    if (st.ema100.ready()) vals.emplace_back("ema100", st.ema100.value());
    if (st.stoch.ready()) {
        vals.emplace_back("stoch_k", st.stoch.k());
        vals.emplace_back("stoch_d", st.stoch.d());
    }

    auto swing_vals = st.swing.get_indicator_values();
    vals.insert(vals.end(), swing_vals.begin(), swing_vals.end());

    auto regime_vals = st.regime.get_indicator_values();
    vals.insert(vals.end(), regime_vals.begin(), regime_vals.end());

    vals.emplace_back("cont_phase", static_cast<double>(static_cast<int>(st.phase)));
    vals.emplace_back("orientation_bias", static_cast<double>(st.orientation_bias));

    return vals;
}

void structure_continuation_strategy::reset(uint64_t /*seed*/)
{
    for (auto& [sym, st] : states_)
    {
        // Reconstruct indicators (they do not all have reset() yet)
        st.ema50  = exponential_moving_average(50);
        st.ema100 = exponential_moving_average(100);
        st.stoch  = stochastic_oscillator(5, 3, 3);
        st.swing  = swing_detector(swing_strength_, swing_history_);
        st.atr    = average_true_range(14);
        st.regime = ema_regime_detector(14, 48, 1.65, 2.8, 1.9);

        st.phase = SymbolState::ContinuationPhase::NORMAL;
        st.orientation_bias = 0;
        st.bars_since_sideways_exit = 0;
        st.signals_since_orientation = 0;
        st.position_open = false;
        st.open_lots = 0;
        st.last_opener_id = 0;
    }

    // Clear strategy-level pending exit intents (MC safety)
    pending_intents_.clear();
}
