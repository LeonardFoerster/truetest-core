#include "mean_reversion_strategy.h"
#include "strategy_registry.h"
#include "../core/event.h"

#include <cmath>
#include <optional>

REGISTER_STRATEGY("mean-reversion", []() {
    return std::make_shared<mean_reversion_strategy>();
})

mean_reversion_strategy::mean_reversion_strategy(std::size_t period, double equity,
                                                   double risk_fraction, double sl_pct, double tp_pct,
                                                   std::size_t atr_period)
    : period_(period), equity_(equity), risk_fraction_(risk_fraction),
      sl_pct_(sl_pct), tp_pct_(tp_pct), atr_period_(atr_period) {}

double mean_reversion_strategy::compute_quantity(double price) const
{
    if (price <= 0.0) return 0.0;
    return equity_ * risk_fraction_ / price;
}

// Phase 4 #1: True fixed-risk sizing based on actual stop distance
double mean_reversion_strategy::compute_quantity_with_sl(double entry, double sl_price) const
{
    double risk_distance = std::abs(entry - sl_price);
    if (risk_distance < 1e-8) risk_distance = 0.01 * entry; // safety floor
    return (equity_ * risk_fraction_) / risk_distance;
}

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

average_true_range& mean_reversion_strategy::get_atr(const std::string& symbol)
{
    auto it = atrs_.find(symbol);
    if (it == atrs_.end())
    {
        atrs_.emplace(symbol, average_true_range(atr_period_));
        return atrs_.at(symbol);
    }
    return it->second;
}

swing_detector& mean_reversion_strategy::get_swing(const std::string& symbol)
{
    auto it = swings_.find(symbol);
    if (it == swings_.end())
    {
        swings_.emplace(symbol, swing_detector(swing_strength_, swing_history_));
        return swings_.at(symbol);
    }
    return it->second;
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

    (void)get_atr(mkt.get_symbol()).update(mkt.get_high(), mkt.get_low(), mkt.get_close());
    (void)get_swing(mkt.get_symbol()).update(mkt.get_high(), mkt.get_low(), mkt.get_close());

    const double price = mkt.get_close();

    if (price < *sma_value)
    {
        double entry = price;

        // Phase 4 #1: Compute real SL first for correct risk-based sizing
        double tentative_sl = compute_intended_sl(mkt.get_symbol(), entry, true);
        double qty = compute_quantity_with_sl(entry, tentative_sl);
        if (qty <= 0.0) return std::nullopt;

        auto intents = create_exit_intents(mkt.get_symbol(), entry, qty, true);
        pending_intents_.insert(pending_intents_.end(), intents.begin(), intents.end());
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy, qty, entry);
    }

    if (price > *sma_value)
    {
        double entry = price;

        double tentative_sl = compute_intended_sl(mkt.get_symbol(), entry, false);
        double qty = compute_quantity_with_sl(entry, tentative_sl);
        if (qty <= 0.0) return std::nullopt;

        auto intents = create_exit_intents(mkt.get_symbol(), entry, qty, false);
        pending_intents_.insert(pending_intents_.end(), intents.begin(), intents.end());
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::sell, qty, entry);
    }

    return std::nullopt;
}

std::optional<order_event> mean_reversion_strategy::on_tick(const tick_event& te)
{
    auto& sma = get_sma(te.get_symbol());
    auto sma_value = sma.update(te.get_price());
    if (!sma_value) return std::nullopt;

    const double price = te.get_price();
    (void)get_atr(te.get_symbol()).update(price, price, price);
    (void)get_swing(te.get_symbol()).update(price, price, price);

    if (price < *sma_value)
    {
        double entry = price;

        double tentative_sl = compute_intended_sl(te.get_symbol(), entry, true);
        double qty = compute_quantity_with_sl(entry, tentative_sl);
        if (qty <= 0.0) return std::nullopt;

        auto intents = create_exit_intents(te.get_symbol(), entry, qty, true);
        pending_intents_.insert(pending_intents_.end(), intents.begin(), intents.end());
        return order_event(te.get_timestamp(), te.get_symbol(),
                           order_type::market, order_side::buy, qty, entry);
    }

    if (price > *sma_value)
    {
        double entry = price;

        double tentative_sl = compute_intended_sl(te.get_symbol(), entry, false);
        double qty = compute_quantity_with_sl(entry, tentative_sl);
        if (qty <= 0.0) return std::nullopt;

        auto intents = create_exit_intents(te.get_symbol(), entry, qty, false);
        pending_intents_.insert(pending_intents_.end(), intents.begin(), intents.end());
        return order_event(te.get_timestamp(), te.get_symbol(),
                           order_type::market, order_side::sell, qty, entry);
    }

    return std::nullopt;
}

std::vector<truetest::exits::exit_intent>
mean_reversion_strategy::take_pending_exit_intents()
{
    auto out = std::move(pending_intents_);
    pending_intents_.clear();
    return out;
}

void mean_reversion_strategy::set_position_open(const std::string& /*symbol*/, bool /*open*/)
{
    // Pyramiding enabled - no longer blocking on position state.
}



std::vector<std::pair<std::string, double>>
mean_reversion_strategy::get_indicator_values(const std::string& symbol) const
{
    std::vector<std::pair<std::string, double>> vals;

    auto sma_it = smas_.find(symbol);
    if (sma_it != smas_.end() && sma_it->second.ready())
        vals.emplace_back("sma_" + std::to_string(period_), sma_it->second.value());

    auto atr_it = atrs_.find(symbol);
    if (atr_it != atrs_.end() && atr_it->second.ready())
        vals.emplace_back("atr_" + std::to_string(atr_period_), atr_it->second.value());

    auto swing_it = swings_.find(symbol);
    if (swing_it != swings_.end() && swing_it->second.ready())
    {
        vals.emplace_back("swing_phase", static_cast<double>(static_cast<int>(swing_it->second.phase())));
    }

    // Phase 4 #5 diagnostics could be added here (last computed impulse range, effective R:R, etc.)
    // For a production version we would store the last computed values per symbol.

    return vals;
}

// Phase 4 #1 + #2: Compute the intended stop loss price for sizing and quality decisions
double mean_reversion_strategy::compute_intended_sl(const std::string& symbol, double entry, bool is_long) const
{
    auto& atr   = const_cast<mean_reversion_strategy*>(this)->get_atr(symbol);
    auto& swing = const_cast<mean_reversion_strategy*>(this)->get_swing(symbol);

    const bool use_fib = (exit_style_ == "fib");
    const bool use_atr = (exit_style_ == "atr");

    if (use_fib && swing.ready() && atr.ready())
    {
        auto opposing = is_long ? swing.last_confirmed_swing_high() : swing.last_confirmed_swing_low();

        double impulse_high = is_long ? (opposing ? opposing->price : entry * 1.03) : entry;
        double impulse_low  = is_long ? entry : (opposing ? opposing->price : entry * 0.97);

        double impulse_range = std::abs(impulse_high - impulse_low);

        // Phase 4 #2: Quality filter - discard weak impulses
        const double atrv = atr.value();
        if (impulse_range < 1.2 * atrv)
        {
            // Fall back to simple ATR stop for weak structure
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
            // Simple structural / ATR-based SL (no external fib helpers available)
            double sl = entry - (sl_atr_mult_ * atrv);
            if (opposing) sl = std::min(sl, opposing->price - 0.2 * atrv);
            if (entry - sl < min_dist) sl = entry - min_dist;
            return sl;
        }
        else
        {
            double sl = entry + (sl_atr_mult_ * atrv);
            if (opposing) sl = std::max(sl, opposing->price + 0.2 * atrv);
            if (sl - entry < min_dist) sl = entry + min_dist;
            return sl;
        }
    }

    if ((use_atr || use_fib) && atr.ready() && atr.value() > 0.0)
    {
        return is_long ? entry - (sl_atr_mult_ * atr.value())
                       : entry + (sl_atr_mult_ * atr.value());
    }

    // Legacy pct fallback
    return is_long ? entry * (1.0 - sl_pct_)
                   : entry * (1.0 + sl_pct_);
}

// Phase 3 Polish: Returns one or more exit intents.
// Supports clean mode selection via exit_style_ ("pct", "atr", "fib").
// In "fib" mode: structural impulse leg + Fib ratios + ATR safety floor + optional scale-out + trailing runner.
/**
 * Phase 3: Zentrale Exit-Erzeugung mit sauberer Modus-Auswahl.
 *
 * exit_style_:
 *   "pct"  → klassische feste Prozent (sl_pct / tp_pct)
 *   "atr"  → ATR-basierte Stops/Targets (sl_atr_mult / tp_atr_mult als R-Multiple)
 *   "fib"  → Struktur-basiert mit Fibonacci (Swing + Fib-Ratios + ATR-Puffer + Safety-Floors)
 *
 * Im Fib-Modus werden bei aktivem scale_out_ratio_ automatisch zwei Intents erzeugt:
 *   1. Partieller Close am Fib-TP
 *   2. Runner mit Trailing-Stop
 */
std::vector<truetest::exits::exit_intent>
mean_reversion_strategy::create_exit_intents(const std::string& symbol,
                                             double entry,
                                             double qty,
                                             bool is_long)
{
    using namespace truetest::exits;

    std::vector<exit_intent> result;

    // Compute base SL/TP. Only use stable exits API (make_long/make_short + direct struct fill).
    // Advanced fib/ATR maker functions were part of an incomplete rewrite and are not available.
    auto base = [this, &symbol, entry, qty, is_long]() -> std::optional<exit_intent> {
        auto& atr   = get_atr(symbol);
        auto& swing = get_swing(symbol);

        const bool want_fib = (exit_style_ == "fib");
        const bool want_atr = (exit_style_ == "atr") || (exit_style_ == "fib"); // fib also benefits from ATR

        if (want_fib && swing.ready() && atr.ready())
        {
            auto opposing = is_long ? swing.last_confirmed_swing_high() : swing.last_confirmed_swing_low();

            double impulse_high = is_long ? (opposing ? opposing->price : entry * 1.03) : entry;
            double impulse_low  = is_long ? entry : (opposing ? opposing->price : entry * 0.97);

            if (std::abs(impulse_high - impulse_low) < 0.001 * entry)
            {
                impulse_high = is_long ? entry * 1.02 : entry;
                impulse_low  = is_long ? entry : entry * 0.98;
            }

            const double atrv = atr.value();
            double min_dist   = std::max(0.4 * atrv, 0.003 * entry);

            exit_intent ei;
            ei.symbol        = symbol;
            ei.close_side    = is_long ? order_side::sell : order_side::buy;
            ei.qty           = qty;
            ei.strategy_name = "mean-reversion";

            if (is_long)
            {
                double sl = entry - (sl_atr_mult_ * atrv);
                if (opposing) sl = std::min(sl, opposing->price - 0.2 * atrv);
                if (entry - sl < min_dist) sl = entry - min_dist;

                // Simple structural TP using impulse extension + ATR floor
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

        if (atr.ready() && atr.value() > 0.0)
        {
            // ATR-based exits using the stable make_* helpers + direct fill for scale-outs later
            exit_intent ei;
            ei.symbol        = symbol;
            ei.close_side    = is_long ? order_side::sell : order_side::buy;
            ei.qty           = qty;
            ei.strategy_name = "mean-reversion";

            const double atrv = atr.value();
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

        // Safe legacy pct fallback (always available)
        return is_long
            ? truetest::exits::make_long_exit_intent(symbol, entry, qty, sl_pct_, tp_pct_, "mean-reversion")
            : truetest::exits::make_short_exit_intent(symbol, entry, qty, sl_pct_, tp_pct_, "mean-reversion");
    }();

    if (!base) return result;

    bool do_scale = (exit_style_ == "fib" || exit_style_ == "atr") && scale_out_ratio_ > 0.0 && scale_out_ratio_ < 1.0;

    if (do_scale)
    {
        exit_intent first = *base;
        first.qty_fraction = scale_out_ratio_;
        result.push_back(std::move(first));

        exit_intent runner = *base;
        runner.qty_fraction = 1.0 - scale_out_ratio_;
        runner.take_profit.reset();

        // Phase 4 #3: Use ATR-based Chandelier-style trailing instead of fixed %
        double atrv = get_atr(symbol).value();
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
        {"risk_fraction", risk_fraction_, 0, 1, "Fraction of equity per trade"},
        {"sl_pct", sl_pct_, 0, 1, "Stop loss % (legacy)"},
        {"tp_pct", tp_pct_, 0, 1, "Take profit % (legacy)"},
        {"atr_period", static_cast<double>(atr_period_), 5, 100, "ATR period"},
        {"sl_atr_mult", sl_atr_mult_, 0.1, 10.0, "SL in ATR units"},
        {"tp_atr_mult", tp_atr_mult_, 0.1, 20.0, "TP as R-multiple"},
        {"swing_strength", static_cast<double>(swing_strength_), 1, 5, "Swing strength"},
        {"fib_sl_retracement", fib_sl_retracement_, 0.1, 0.9, "Fib SL level"},
        {"fib_tp_extension", fib_tp_extension_, 1.0, 3.0, "Fib TP extension"},
        {"atr_buffer_mult_sl", atr_buffer_mult_sl_, 0.0, 1.0, "ATR buffer SL"},
        {"atr_buffer_mult_tp", atr_buffer_mult_tp_, 0.0, 1.0, "ATR buffer TP"},
        {"exit_style", 0.0, 0.0, 0.0, "pct | atr | fib"},
        {"scale_out_ratio", scale_out_ratio_, 0.0, 1.0, "Fraction at first TP"},
        {"trail_atr_mult", trail_atr_mult_, 0.5, 5.0, "Trailing ATR mult for runner"},
    };
}

void mean_reversion_strategy::set_param(const std::string& key, double value)
{
    if (key == "period") { period_ = static_cast<std::size_t>(value); smas_.clear(); }
    else if (key == "equity") equity_ = value;
    else if (key == "risk_fraction") risk_fraction_ = value;
    else if (key == "sl_pct") sl_pct_ = value;
    else if (key == "tp_pct") tp_pct_ = value;
    else if (key == "atr_period") { atr_period_ = static_cast<std::size_t>(value); atrs_.clear(); }
    else if (key == "sl_atr_mult") sl_atr_mult_ = value;
    else if (key == "tp_atr_mult") tp_atr_mult_ = value;
    else if (key == "swing_strength") { swing_strength_ = static_cast<std::size_t>(value); swings_.clear(); }
    else if (key == "fib_sl_retracement") fib_sl_retracement_ = value;
    else if (key == "fib_tp_extension") fib_tp_extension_ = value;
    else if (key == "atr_buffer_mult_sl") atr_buffer_mult_sl_ = value;
    else if (key == "atr_buffer_mult_tp") atr_buffer_mult_tp_ = value;
    else if (key == "exit_style") {
        if (value == 0.0) exit_style_ = "pct";
        else if (value == 1.0) exit_style_ = "atr";
        else exit_style_ = "fib";
    }
    else if (key == "scale_out_ratio") scale_out_ratio_ = value;
    else if (key == "trail_atr_mult") trail_atr_mult_ = value;
    else throw std::runtime_error("Unknown parameter: " + key);
}

void mean_reversion_strategy::reset(uint64_t /*seed*/)
{
    // Clear per-symbol indicator state so the next MC trial starts fresh.
    smas_.clear();
    atrs_.clear();
    swings_.clear();
    pending_intents_.clear();
}
