#include "breakout_strategy.h"
#include "../execution/position_sizing.h"
#include "strategy_registry.h"
#include "../core/event.h"

#include <algorithm>
#include <cmath>
#include <optional>

REGISTER_STRATEGY("breakout", []() {
    return std::make_shared<breakout_strategy>();
})

namespace {
    constexpr std::size_t MAX_HISTORY = 60;
    constexpr double MIN_BODY_STRENGTH = 0.55;
}

breakout_strategy::breakout_strategy(double equity,
                                     double risk_fraction,
                                     std::size_t atr_period,
                                     std::size_t vol_period,
                                     std::size_t lookback,
                                     double breakout_threshold,
                                     double atr_expansion,
                                     double vol_mult,
                                     double min_rr)
    : equity_(equity)
    , risk_fraction_(risk_fraction)
    , atr_period_(atr_period)
    , vol_period_(vol_period)
    , lookback_(lookback)
    , breakout_threshold_(breakout_threshold)
    , atr_expansion_(atr_expansion)
    , vol_mult_(vol_mult)
    , min_rr_(min_rr)
    , states_([this]() {
          SymbolState st;
          st.atr = average_true_range(atr_period_);
          st.vol_sma = simple_moving_average(vol_period_);
          return st;
      })
{
}

void breakout_strategy::trim_deques(SymbolState& st)
{
    while (st.highs.size() > MAX_HISTORY) st.highs.pop_front();
    while (st.lows.size() > MAX_HISTORY) st.lows.pop_front();
    while (st.atr_history.size() > 20) st.atr_history.pop_front();
    while (st.vol_history.size() > 30) st.vol_history.pop_front();
}

double breakout_strategy::get_recent_atr_min(const SymbolState& st, std::size_t n) const
{
    if (st.atr_history.size() < n) return st.atr.value();
    double mn = std::numeric_limits<double>::max();
    auto it = st.atr_history.rbegin();
    for (std::size_t i = 0; i < n && it != st.atr_history.rend(); ++i, ++it)
        mn = std::min(mn, *it);
    return mn;
}

bool breakout_strategy::detect_consolidation(const SymbolState& st, double& out_high, double& out_low) const
{
    if (st.highs.size() < lookback_ || st.lows.size() < lookback_)
        return false;

    // Compute range over lookback window
    auto h_it = st.highs.rbegin();
    auto l_it = st.lows.rbegin();
    double hh = *h_it;
    double ll = *l_it;
    ++h_it; ++l_it;

    std::size_t cnt = 1;
    for (; cnt < lookback_ && h_it != st.highs.rend() && l_it != st.lows.rend(); ++cnt, ++h_it, ++l_it)
    {
        hh = std::max(hh, *h_it);
        ll = std::min(ll, *l_it);
    }

    if (hh <= ll) return false;

    // Simple consolidation proxy: range must be reasonably tight vs recent ATR
    double range = hh - ll;
    double atr = st.atr.ready() ? st.atr.value() : 0.0;
    if (atr > 0.0 && range > 4.5 * atr) // too wide for "coiled"
        return false;

    // Require some contraction: current ATR near its recent low
    if (st.atr_history.size() >= 5)
    {
        double atr_min = get_recent_atr_min(st, 5);
        if (atr > atr_min * 1.35) // not sufficiently contracted
            return false;
    }

    out_high = hh;
    out_low = ll;
    return true;
}

bool breakout_strategy::check_breakout_gates(const SymbolState& st,
                                             double open, double close, double high, double low,
                                             double atr, double vol,
                                             double& out_break_level) const
{
    if (!st.atr.ready() || st.vol_sma.ready() == false || atr <= 0.0)
        return false;

    // Must have consolidation range recorded
    if (st.consolidation_high <= st.consolidation_low)
        return false;

    double upper = st.consolidation_high;
    double vol_avg = st.vol_sma.value();

    // 1. Close >= 0.75% above upper boundary
    double break_price = upper * (1.0 + breakout_threshold_);
    if (close < break_price)
        return false;

    // 2. Strong candle body (>=55% of bar range, conviction close)
    double bar_range = high - low;
    if (bar_range <= 0.0) return false;
    double body = std::abs(close - open);
    if ((body / bar_range) < MIN_BODY_STRENGTH) return false;

    // 3. ATR expansion >15% above its 10-bar low
    double atr_low = get_recent_atr_min(st, 10);
    if (atr <= atr_low * (1.0 + atr_expansion_))
        return false;

    // 4. Volume surge
    if (vol_avg > 0.0 && vol < vol_avg * vol_mult_)
        return false;

    out_break_level = break_price;
    return true;
}

double breakout_strategy::compute_quantity(double price, double sl_distance) const
{
    if (price <= 0.0 || sl_distance <= 0.0) return 0.0;
    // Breakout SL is an absolute level below entry (long-only entries).
    truetest::risk::risk_size_inputs in;
    in.equity            = equity_;
    in.risk_fraction     = risk_fraction_;
    in.entry_price       = price;
    in.stop_price        = price - sl_distance;
    in.is_long           = true;
    in.entry_fee_rate    = entry_fee_rate_;
    in.exit_fee_rate     = exit_fee_rate_;
    in.entry_slip_bps    = entry_slip_bps_;
    in.exit_slip_bps     = exit_slip_bps_;
    in.fixed_fee_per_leg = fixed_fee_per_leg_;
    in.max_notional_frac = max_notional_frac_;
    return truetest::risk::compute_risk_quantity(in);
}

std::optional<order_event> breakout_strategy::on_market(const market_event& mkt)
{
    double o = mkt.get_open();
    double h = mkt.get_high();
    double l = mkt.get_low();
    double c = mkt.get_close();
    int64_t v = mkt.get_volume();

    auto slot = states_.get(mkt.get_symbol());
    auto& st = slot.state;
    const std::string& sym = slot.symbol;

    // Update indicators
    auto atr_opt = st.atr.update(h, l, c);
    (void)st.vol_sma.update(static_cast<double>(v));

    // Maintain history
    st.highs.push_back(h);
    st.lows.push_back(l);
    if (atr_opt) st.atr_history.push_back(*atr_opt);
    st.vol_history.push_back(static_cast<double>(v));
    trim_deques(st);

    if (!st.atr.ready() || !st.vol_sma.ready())
        return std::nullopt;

    double atrv = st.atr.value();

    // Update consolidation detection
    double cons_h = 0.0, cons_l = 0.0;
    bool in_consol = detect_consolidation(st, cons_h, cons_l);

    if (in_consol)
    {
        if (st.phase == SymbolState::Phase::SCANNING)
            st.phase = SymbolState::Phase::CONSOLIDATING;
        st.consolidation_high = cons_h;
        st.consolidation_low = cons_l;
    }

    bool is_open = st.position_open || (st.open_lots > 0);
    if (is_open)
    {
        // While open, strategy is silent; exits are handled by ExitManager.
        // Optionally could add time stop or other logic here.
        return std::nullopt;
    }

    // Check for breakout gates
    double break_lvl = 0.0;
    bool gates_ok = check_breakout_gates(st, o, c, h, l, atrv, static_cast<double>(v), break_lvl);

    if (gates_ok && (st.phase == SymbolState::Phase::CONSOLIDATING || st.phase == SymbolState::Phase::BROKEN))
    {
        // Prefer retest: if just broken, require at least one hold bar above level
        if (st.phase == SymbolState::Phase::BROKEN && st.bars_since_break < 1)
        {
            st.phase = SymbolState::Phase::BROKEN;
            st.breakout_level = break_lvl;
            st.bars_since_break = 1;
            return std::nullopt; // wait one more bar for retest confirmation
        }

        // Compute risk distance per guide: below consol low - 0.5..0.8 ATR
        double sl_price = st.consolidation_low - 0.65 * atrv;
        double sl_distance = c - sl_price;
        if (sl_distance < 0.3 * atrv) sl_distance = 0.8 * atrv; // safety floor

        double qty = compute_quantity(c, sl_distance);
        if (qty <= 0.0) return std::nullopt;

        // Measured move target
        double pattern_h = st.consolidation_high - st.consolidation_low;
        double tp1 = c + pattern_h + 0.6 * atrv;   // guide: measured + 0.5-1x ATR

        // Ensure min R:R
        double rr = (tp1 - c) / sl_distance;
        if (rr < min_rr_)
        {
            // fall back to ATR multiple target
            tp1 = c + 2.8 * sl_distance; // ~2.8R
            rr = 2.8;
        }

        // Approx chandelier trail % from 2x ATR
        double trail_pct = std::clamp( (2.0 * atrv / c) , 0.004, 0.025 );

        // Build two scale-out intents (TP1 + remainder trail)
        truetest::exits::exit_intent tp_part;
        tp_part.symbol        = sym;
        tp_part.close_side    = order_side::sell;
        tp_part.qty           = qty;                 // overwritten by engine using fraction
        tp_part.qty_fraction  = 0.45;
        tp_part.stop_loss     = sl_price;
        tp_part.take_profit   = tp1;
        tp_part.strategy_name = "breakout";

        truetest::exits::exit_intent trail_part;
        trail_part.symbol        = sym;
        trail_part.close_side    = order_side::sell;
        trail_part.qty           = qty;
        trail_part.qty_fraction  = 0.55;
        trail_part.stop_loss     = sl_price;
        trail_part.trailing_pct  = trail_pct;
        trail_part.strategy_name = "breakout";

        pending_intents_.clear();
        pending_intents_.push_back(std::move(tp_part));
        pending_intents_.push_back(std::move(trail_part));

        // Update state
        st.phase = SymbolState::Phase::RETESTED;
        st.breakout_level = break_lvl;
        st.atr_at_break = atrv;
        st.bars_since_break = 0;

        return order_event(mkt.get_timestamp(), sym,
                           order_type::market, order_side::buy, qty, c);
    }

    // Advance breakout state if we saw a clean break but didn't enter yet
    if (st.consolidation_high > 0.0 && c > st.consolidation_high * (1.0 + breakout_threshold_ * 0.6))
    {
        if (st.phase == SymbolState::Phase::CONSOLIDATING)
        {
            st.phase = SymbolState::Phase::BROKEN;
            st.breakout_level = c;
            st.bars_since_break = 0;
        }
        else if (st.phase == SymbolState::Phase::BROKEN)
        {
            st.bars_since_break++;
            if (st.bars_since_break > 8) // stale breakout, reset
            {
                st.phase = SymbolState::Phase::SCANNING;
                st.consolidation_high = 0.0;
                st.consolidation_low = 0.0;
            }
        }
    }
    else
    {
        // No recent break pressure — slowly allow re-scan
        if (st.phase == SymbolState::Phase::BROKEN || st.phase == SymbolState::Phase::RETESTED)
        {
            if (++st.bars_since_break > 12)
            {
                st.phase = SymbolState::Phase::SCANNING;
                st.consolidation_high = 0.0;
                st.consolidation_low = 0.0;
            }
        }
    }

    return std::nullopt;
}

std::vector<truetest::exits::exit_intent>
breakout_strategy::take_pending_exit_intents()
{
    auto out = std::move(pending_intents_);
    pending_intents_.clear();
    return out;
}

void breakout_strategy::on_fill(const fill_event& fill, std::uint64_t opener_order_id)
{
    // Track lots so we know when flat (for re-entry permission)
    auto& st = states_.get(fill.get_symbol()).state;

    const bool is_opener = (opener_order_id == fill.get_order_id());
    if (is_opener)
    {
        if (fill.get_side() == order_side::buy)
            ++st.open_lots;
        // shorts not primary for this strategy
    }
    else
    {
        // closer
        if (st.open_lots > 0)
            --st.open_lots;
        if (st.open_lots == 0)
        {
            st.position_open = false;
            st.phase = SymbolState::Phase::SCANNING; // ready to look for next setup
            st.consolidation_high = 0.0;
            st.consolidation_low = 0.0;
        }
    }
}

void breakout_strategy::set_position_open(const std::string& symbol, bool open)
{
    auto& st = states_.get(symbol).state;
    st.position_open = open;
    if (!open && st.open_lots > 0)
        st.open_lots = 0;
}

std::vector<param_def> breakout_strategy::get_param_schema() const
{
    return {
        {"equity", equity_, 1.0, 1e12, "Account equity for 0.5% risk sizing"},
        {"risk_fraction", risk_fraction_, 0.0001, 0.05, "Stop-risk budget as fraction of equity"},
        {"entry_fee_rate", entry_fee_rate_, 0, 0.05, "Entry fee as fraction of notional"},
        {"exit_fee_rate", exit_fee_rate_, 0, 0.05, "Exit fee as fraction of notional"},
        {"entry_slip_bps", entry_slip_bps_, 0, 500, "Adverse entry slippage (bps)"},
        {"exit_slip_bps", exit_slip_bps_, 0, 500, "Adverse exit/stop slippage (bps)"},
        {"fixed_fee_per_leg", fixed_fee_per_leg_, 0, 1e6, "Fixed fee per fill leg"},
        {"max_notional_frac", max_notional_frac_, 0, 10, "Optional max position notional / equity (0=off)"},
        {"atr_period", static_cast<double>(atr_period_), 5, 100, "ATR period"},
        {"vol_period", static_cast<double>(vol_period_), 5, 100, "Volume SMA period"},
        {"lookback", static_cast<double>(lookback_), 5, 200, "Consolidation lookback bars"},
        {"breakout_threshold", breakout_threshold_, 0.001, 0.05, "Breakout % above range (0.0075)"},
        {"atr_expansion", atr_expansion_, 0.05, 0.5, "ATR expansion threshold (0.15)"},
        {"vol_mult", vol_mult_, 1.0, 5.0, "Volume multiplier for surge (1.8)"},
        {"min_rr", min_rr_, 1.0, 10.0, "Minimum reward:risk to take trade"},
    };
}

void breakout_strategy::set_param(const std::string& key, double value)
{
    if (key == "equity") equity_ = value;
    else if (key == "risk_fraction") risk_fraction_ = value;
    else if (key == "entry_fee_rate") entry_fee_rate_ = value;
    else if (key == "exit_fee_rate") exit_fee_rate_ = value;
    else if (key == "entry_slip_bps") entry_slip_bps_ = value;
    else if (key == "exit_slip_bps") exit_slip_bps_ = value;
    else if (key == "fixed_fee_per_leg") fixed_fee_per_leg_ = value;
    else if (key == "max_notional_frac") max_notional_frac_ = value;
    else if (key == "atr_period") { atr_period_ = static_cast<std::size_t>(value); states_.clear(); }
    else if (key == "vol_period") { vol_period_ = static_cast<std::size_t>(value); states_.clear(); }
    else if (key == "lookback") lookback_ = static_cast<std::size_t>(value);
    else if (key == "breakout_threshold") breakout_threshold_ = value;
    else if (key == "atr_expansion") atr_expansion_ = value;
    else if (key == "vol_mult") vol_mult_ = value;
    else if (key == "min_rr") min_rr_ = value;
    else throw std::runtime_error("Unknown parameter for breakout: " + key);
}

std::vector<std::pair<std::string, double>> breakout_strategy::get_indicator_values(
    const std::string& symbol) const
{
    std::vector<std::pair<std::string, double>> vals;
    const auto* st_ptr = states_.find(symbol);
    if (!st_ptr) return vals;

    const auto& st = *st_ptr;
    if (st.atr.ready())
        vals.emplace_back("atr_" + std::to_string(atr_period_), st.atr.value());
    if (st.vol_sma.ready())
        vals.emplace_back("vol_sma_" + std::to_string(vol_period_), st.vol_sma.value());

    if (!st.highs.empty() && !st.lows.empty())
    {
        vals.emplace_back("consol_high", st.consolidation_high);
        vals.emplace_back("consol_low", st.consolidation_low);
    }
    return vals;
}
