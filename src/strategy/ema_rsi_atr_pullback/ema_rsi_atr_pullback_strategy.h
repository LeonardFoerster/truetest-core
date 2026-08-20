#pragma once

#include "../../core/event.h"
#include "../../indicator/atr.h"
#include "../../indicator/ema.h"
#include "../../indicator/rsi.h"
#include "exits/exit_intent.h"
#include "../strategy_interface.h"
#include "../symbol_state_store.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * EMA-150 / RSI-14 / ATR Pullback Strategy.
 *
 * Systematic trend-pullback strategy:
 * - Trend filter: EMA-150 (close > EMA: uptrend, close < EMA: downtrend).
 * - Pullback trigger: RSI-14 crossing back over 40 from below for long,
 *   crossing back under 60 from above for short.
 * - Sizing: Cost-aware fixed-risk sizing (default 0.5% equity per trade).
 * - Protection: 2.0 * ATR initial stop registered as exit_intent with reference_entry.
 * - Trend exit: Signal close across EMA (close < EMA for long, close > EMA for short).
 * - Single active trade/entry per symbol (no pyramiding).
 */
class ema_rsi_atr_pullback_strategy : public IStrategy
{
public:
    enum class trade_state : uint8_t
    {
        flat = 0,
        entry_pending_long,
        entry_pending_short,
        long_open,
        short_open,
        exit_pending_long,
        exit_pending_short
    };

    explicit ema_rsi_atr_pullback_strategy(
        std::size_t ema_period = 150,
        std::size_t rsi_period = 14,
        std::size_t atr_period = 14,
        double risk_fraction = 0.005,
        double atr_stop_multiplier = 2.0,
        double equity = 10000.0,
        double long_rsi_threshold = 40.0,
        double short_rsi_threshold = 60.0);

    std::optional<order_event> on_market(const market_event& mkt) override;

    void set_position_open(const std::string& symbol, bool open) override;
    void set_account_equity(double equity) override { equity_ = equity; }

    void on_fill(const fill_event& fill, std::uint64_t opener_order_id) override;

    std::vector<truetest::exits::exit_intent> take_pending_exit_intents() override;

    std::vector<param_def> get_param_schema() const override;
    void set_param(const std::string& key, double value) override;

    std::vector<std::pair<std::string, double>> get_indicator_values(
        const std::string& symbol) const override;

    void reset(uint64_t seed = 0) override;
    bool supports_mc_trial_reuse() const override { return true; }

    // Helpers / accessors
    trade_state get_trade_state(const std::string& symbol) const;
    double get_open_qty(const std::string& symbol) const;
    std::uint64_t get_opener_order_id(const std::string& symbol) const;

private:
    struct SymbolState
    {
        exponential_moving_average ema;
        relative_strength_index    rsi;
        average_true_range         atr;

        std::optional<double> prev_rsi;

        trade_state   state = trade_state::flat;
        double        open_qty = 0.0;
        std::uint64_t opener_order_id = 0;
        double        expected_entry_qty = 0.0;

        SymbolState(std::size_t ema_p, std::size_t rsi_p, std::size_t atr_p)
            : ema(ema_p), rsi(rsi_p), atr(atr_p)
        {}
    };

    static bool is_valid_bar(const market_event& mkt);
    bool has_active_trade() const;
    void reinit_symbol_states();

    std::size_t ema_period_ = 150;
    std::size_t rsi_period_ = 14;
    std::size_t atr_period_ = 14;

    double long_rsi_threshold_  = 40.0;
    double short_rsi_threshold_ = 60.0;
    double atr_stop_multiplier_ = 2.0;

    double equity_        = 10000.0;
    double risk_fraction_ = 0.005;

    bool allow_long_  = true;
    bool allow_short_ = true;

    double entry_fee_rate_    = 0.0;
    double exit_fee_rate_     = 0.0;
    double entry_slip_bps_    = 0.0;
    double exit_slip_bps_     = 0.0;
    double fixed_fee_per_leg_ = 0.0;
    double max_notional_frac_ = 0.0;
    double quantity_step_     = 0.0;

    SymbolStateStore<SymbolState> states_;
    std::vector<truetest::exits::exit_intent> pending_intents_;
};
