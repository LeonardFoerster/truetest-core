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
        double qty = compute_quantity(price);
        if (qty <= 0.0) return std::nullopt;

        double entry = price;
        auto intents = create_exit_intents(mkt.get_symbol(), entry, qty, true);
        pending_intents_.insert(pending_intents_.end(), intents.begin(), intents.end());
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy, qty, entry);
    }

    if (price > *sma_value)
    {
        double qty = compute_quantity(price);
        if (qty <= 0.0) return std::nullopt;

        double entry = price;
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
        double qty = compute_quantity(price);
        if (qty <= 0.0) return std::nullopt;

        double entry = price;
        auto intents = create_exit_intents(te.get_symbol(), entry, qty, true);
        pending_intents_.insert(pending_intents_.end(), intents.begin(), intents.end());
        return order_event(te.get_timestamp(), te.get_symbol(),
                           order_type::market, order_side::buy, qty, entry);
    }

    if (price > *sma_value)
    {
        double qty = compute_quantity(price);
        if (qty <= 0.0) return std::nullopt;

        double entry = price;
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

    return vals;
}

std::optional<truetest::exits::exit_intent>
mean_reversion_strategy::create_exit_intent(const std::string& symbol,
                                            double entry,
                                            double qty,
                                            bool is_long)
{
    using namespace truetest::exits;

    auto& atr = const_cast<mean_reversion_strategy*>(this)->get_atr(symbol);
    auto& swing = const_cast<mean_reversion_strategy*>(this)->get_swing(symbol);

    bool use_fib = (exit_style_ == "fib");
    bool use_atr = (exit_style_ == "atr");

    if (use_fib && swing.ready() && atr.ready())
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

        // Safety floor + Confluence
        double min_dist = std::max(0.4 * atrv, 0.003 * entry);

        if (is_long)
        {
            double sl = suggest_long_fib_sl(impulse_high, impulse_low, atrv, fib_sl_retracement_, atr_buffer_mult_sl_);
            if (entry - sl < min_dist) sl = entry - min_dist;

            double tp = suggest_fib_tp(impulse_high, impulse_low, atrv, fib_tp_extension_, atr_buffer_mult_tp_, true);
            if (tp - entry < 1.8 * atrv) tp = entry + 1.8 * atrv;

            exit_intent ei;
            ei.symbol = symbol; ei.close_side = order_side::sell; ei.qty = qty;
            ei.stop_loss = sl; ei.take_profit = tp; ei.strategy_name = "mean-reversion";
            return ei;
        }
        else
        {
            double sl = suggest_short_fib_sl(impulse_high, impulse_low, atrv, fib_sl_retracement_, atr_buffer_mult_sl_);
            if (sl - entry < min_dist) sl = entry + min_dist;

            double tp = suggest_fib_tp(impulse_high, impulse_low, atrv, fib_tp_extension_, atr_buffer_mult_tp_, false);
            if (entry - tp < 1.8 * atrv) tp = entry - 1.8 * atrv;

            exit_intent ei;
            ei.symbol = symbol; ei.close_side = order_side::buy; ei.qty = qty;
            ei.stop_loss = sl; ei.take_profit = tp; ei.strategy_name = "mean-reversion";
            return ei;
        }
    }

    if (use_atr && atr.ready() && atr.value() > 0.0)
    {
        return is_long
            ? make_atr_long_exit_intent(symbol, entry, qty, atr.value(), sl_atr_mult_, tp_atr_mult_, true, "mean-reversion")
            : make_atr_short_exit_intent(symbol, entry, qty, atr.value(), sl_atr_mult_, tp_atr_mult_, true, "mean-reversion");
    }

    if (atr.ready() && atr.value() > 0.0)
    {
        return is_long
            ? truetest::exits::make_atr_long_exit_intent(symbol, entry, qty, atr.value(), sl_atr_mult_, tp_atr_mult_, true, "mean-reversion")
            : truetest::exits::make_atr_short_exit_intent(symbol, entry, qty, atr.value(), sl_atr_mult_, tp_atr_mult_, true, "mean-reversion");
    }

    return is_long
        ? truetest::exits::make_long_exit_intent(symbol, entry, qty, sl_pct_, tp_pct_, "mean-reversion")
        : truetest::exits::make_short_exit_intent(symbol, entry, qty, sl_pct_, tp_pct_, "mean-reversion");
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

    // Compute base SL/TP using existing Fib/ATR/legacy logic (single intent)
    auto base = [this, &symbol, entry, qty, is_long]() -> std::optional<exit_intent> {
        auto& atr  = get_atr(symbol);
        auto& swing = get_swing(symbol);

        if (use_fib_exits_ && swing.ready() && atr.ready())
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

            if (is_long)
            {
                double sl = suggest_long_fib_sl(impulse_high, impulse_low, atrv, fib_sl_retracement_, atr_buffer_mult_sl_);
                double tp = suggest_fib_tp(impulse_high, impulse_low, atrv, fib_tp_extension_, atr_buffer_mult_tp_, true);

                exit_intent ei;
                ei.symbol = symbol; ei.close_side = order_side::sell; ei.qty = qty;
                ei.stop_loss = sl; ei.take_profit = tp; ei.strategy_name = "mean-reversion";
                return ei;
            }
            else
            {
                double sl = suggest_short_fib_sl(impulse_high, impulse_low, atrv, fib_sl_retracement_, atr_buffer_mult_sl_);
                double tp = suggest_fib_tp(impulse_high, impulse_low, atrv, fib_tp_extension_, atr_buffer_mult_tp_, false);

                exit_intent ei;
                ei.symbol = symbol; ei.close_side = order_side::buy; ei.qty = qty;
                ei.stop_loss = sl; ei.take_profit = tp; ei.strategy_name = "mean-reversion";
                return ei;
            }
        }

        if (atr.ready() && atr.value() > 0.0)
        {
            return is_long
                ? truetest::exits::make_atr_long_exit_intent(symbol, entry, qty, atr.value(), sl_atr_mult_, tp_atr_mult_, true, "mean-reversion")
                : truetest::exits::make_atr_short_exit_intent(symbol, entry, qty, atr.value(), sl_atr_mult_, tp_atr_mult_, true, "mean-reversion");
        }

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
        runner.trailing_pct = std::clamp(trail_atr_mult_ * 0.01, 0.003, 0.05);
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
