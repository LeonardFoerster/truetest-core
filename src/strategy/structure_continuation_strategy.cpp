#include "structure_continuation_strategy.h"
#include "strategy_registry.h"
#include "../execution/position_sizing.h"

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
    , states_([this]() {
          SymbolState st;
          st.swing = swing_detector(swing_strength_, swing_history_);
          return st;
      })
{
}

void structure_continuation_strategy::update_all_indicators(
    SymbolState& st,
    double /*open*/, double high, double low, double close)
{
    // Always keep trend/ATR/structure warm so the next setup after flat is valid.
    (void)st.atr.update(high, low, close);
    (void)st.ema50.update(close);
    (void)st.ema100.update(close);
    st.swing.update(high, low, close);
    st.last_close = close;

    // Stoch + regime are only consumed by setup filters / FSM outside of an
    // active trade. Skip them in IN_TRADE / COOLDOWN — largest per-bar win on
    // the structure path (stoch walks 3 deques + 2 SMAs; regime scans swings).
    using Phase = SymbolState::ContinuationPhase;
    if (st.phase == Phase::IN_TRADE || st.phase == Phase::COOLDOWN)
        return;

    (void)st.stoch.update(high, low, close);

    if (st.ema50.ready() && st.ema100.ready() && st.stoch.ready() &&
        st.swing.ready() && st.atr.ready())
    {
        st.regime.update(st.ema50.value(), st.ema100.value(), st.swing, st.atr);
    }
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
        else
        {
            // Re-entered sideways while waiting for first signal → reset orientation
            st.orientation_bias = 0;
            st.signals_since_orientation = 0;
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
        if (st.regime.is_sideways())
        {
            // Sideways returned while we were waiting for the second signal → reset
            st.phase = Phase::AFTER_SIDEWAYS;
            st.orientation_bias = 0;
            st.signals_since_orientation = 0;
            break;
        }

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
        break;

    case Phase::COOLDOWN:
        st.bars_in_cooldown++;
        // After a reasonable cooldown period, allow the strategy to start scanning again
        if (st.bars_in_cooldown >= 8)
        {
            st.phase = Phase::NORMAL;
            st.orientation_bias = 0;
            st.signals_since_orientation = 0;
            st.bars_in_cooldown = 0;
        }
        break;
    }

    // Basic timeout / reset logic
    if (st.phase == Phase::AFTER_SIDEWAYS && st.bars_since_sideways_exit > 30)
    {
        st.phase = Phase::NORMAL;
    }
}

double structure_continuation_strategy::compute_quantity(
    double price, double sl_distance, double equity, bool is_long) const
{
    // Sizing uses provided equity (or 10k fallback for backtests/demos).
    // Structure SL levels are absolute (no reference_entry rebase); expected
    // entry/exit slip and fees are folded into the per-unit risk budget.
    if (!(price > 0.0) || sl_distance <= 0.0) return 0.0;
    if (equity <= 0.0) equity = 10000.0;

    truetest::risk::risk_size_inputs in;
    in.equity            = equity;
    in.risk_fraction     = risk_fraction_;
    in.entry_price       = price;
    in.stop_price        = is_long ? (price - sl_distance) : (price + sl_distance);
    in.is_long           = is_long;
    in.entry_fee_rate    = entry_fee_rate_;
    in.exit_fee_rate     = exit_fee_rate_;
    in.entry_slip_bps    = entry_slip_bps_;
    in.exit_slip_bps     = exit_slip_bps_;
    in.fixed_fee_per_leg = fixed_fee_per_leg_;
    in.max_notional_frac = max_notional_frac_;
    return truetest::risk::compute_risk_quantity(in);
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

    auto slot = states_.get(sym);
    auto& st = slot.state;
    const std::string& interned = slot.symbol;

    update_all_indicators(st, o, h, l, c);

    // Skip setup evaluation while in trade/cooldown — FSM only needs bar
    // counters there, and is_valid_* walks swing/stoch/regime every call.
    using Phase = SymbolState::ContinuationPhase;
    const bool need_signals =
        st.phase != Phase::IN_TRADE && st.phase != Phase::COOLDOWN;

    bool long_sig  = need_signals && is_valid_long_setup(st);
    bool short_sig = need_signals && is_valid_short_setup(st);

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

            double qty = compute_quantity(c, std::max(sl_dist, c * 0.005), 10000.0, go_long);
            if (qty <= 0.0) qty = 1.0; // fallback for backtests / tiny equity

            order_side side = go_long ? order_side::buy : order_side::sell;

            // Mark as consuming the state
            st.phase = SymbolState::ContinuationPhase::IN_TRADE;

            // Real exit intents per user's rules (A1)
            // SL = min(EMA100, previous swing) + buffer (conservative)
            // TP = previous opposing swing, with range heuristic for wig vs close
            // Scale-out + trailing; no BE at entry
            create_exit_intents(interned, st, c, qty, go_long);

            return order_event(mkt.get_timestamp(), interned,
                               order_type::market, side, qty, c);
        }
    }

    return std::nullopt;
}

void structure_continuation_strategy::set_position_open(const std::string& symbol, bool open)
{
    auto& st = states_.get(symbol).state;
    st.position_open = open;
    if (!open)
    {
        st.open_lots = 0;
        // Return to COOLDOWN after a full close. Explicitly clear orientation state (A2 hardening)
        st.phase = SymbolState::ContinuationPhase::COOLDOWN;
        st.orientation_bias = 0;
        st.signals_since_orientation = 0;
        st.bars_in_cooldown = 0;
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
    auto& st = states_.get(fill.get_symbol()).state;

    if (opener_order_id != 0)
    {
        st.last_opener_id = opener_order_id;

        // If this is a closer fill for our last opener, the position for that opener is now closed
        if (opener_order_id == st.last_opener_id && fill.get_order_id() != opener_order_id)
        {
            // A closer fill arrived for our opener → position for this trade is closed
            st.open_lots = 0;
            // Explicitly clear orientation state on full close (A2 hardening)
            st.orientation_bias = 0;
            st.signals_since_orientation = 0;
            st.phase = SymbolState::ContinuationPhase::COOLDOWN;
            st.bars_in_cooldown = 0;
        }
        else if (opener_order_id == fill.get_order_id())
        {
            // Opener fill
            st.open_lots = 1;
        }
    }
}

std::vector<param_def> structure_continuation_strategy::get_param_schema() const
{
    return {
        {"risk_fraction", risk_fraction_, 0.001, 0.05, "Stop-risk budget as fraction of equity"},
        {"entry_fee_rate", entry_fee_rate_, 0, 0.05, "Entry fee as fraction of notional"},
        {"exit_fee_rate", exit_fee_rate_, 0, 0.05, "Exit fee as fraction of notional"},
        {"entry_slip_bps", entry_slip_bps_, 0, 500, "Adverse entry slippage (bps)"},
        {"exit_slip_bps", exit_slip_bps_, 0, 500, "Adverse exit/stop slippage (bps)"},
        {"fixed_fee_per_leg", fixed_fee_per_leg_, 0, 1e6, "Fixed fee per fill leg"},
        {"max_notional_frac", max_notional_frac_, 0, 10, "Optional max position notional / equity (0=off)"},
        {"swing_strength", static_cast<double>(swing_strength_), 1.0, 5.0, "Pivot strength (bars each side)"},
    };
}

void structure_continuation_strategy::set_param(const std::string& key, double value)
{
    if (key == "risk_fraction") risk_fraction_ = value;
    else if (key == "entry_fee_rate") entry_fee_rate_ = value;
    else if (key == "exit_fee_rate") exit_fee_rate_ = value;
    else if (key == "entry_slip_bps") entry_slip_bps_ = value;
    else if (key == "exit_slip_bps") exit_slip_bps_ = value;
    else if (key == "fixed_fee_per_leg") fixed_fee_per_leg_ = value;
    else if (key == "max_notional_frac") max_notional_frac_ = value;
    else if (key == "swing_strength") swing_strength_ = static_cast<std::size_t>(value);
    else throw std::runtime_error("Unknown parameter: " + key);
}

std::vector<std::pair<std::string, double>>
structure_continuation_strategy::get_indicator_values(const std::string& symbol) const
{
    const auto* st_ptr = states_.find(symbol);
    if (!st_ptr) return {};

    const auto& st = *st_ptr;
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
    // Drop all dense slots; factory rebuilds swing with current strength/history.
    states_.clear();
    pending_intents_.clear();
}
