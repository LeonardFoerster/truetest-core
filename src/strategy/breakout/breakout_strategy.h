#pragma once

#include "../../core/event.h"
#include "../../indicator/atr.h"
#include "../../indicator/sma.h"
#include "exits/exit_intent.h"
#include "../strategy_interface.h"
#include "../symbol_state_store.h"

#include <deque>
#include <optional>
#include <string>
#include <vector>

class breakout_strategy : public IStrategy
{
public:
    // Defaults tuned to the Coiled Spring guide for small capital (0.5% risk)
    explicit breakout_strategy(double equity = 2500.0,
                               double risk_fraction = 0.005,
                               std::size_t atr_period = 14,
                               std::size_t vol_period = 20,
                               std::size_t lookback = 20,
                               double breakout_threshold = 0.0075,
                               double atr_expansion = 0.15,
                               double vol_mult = 1.8,
                               double min_rr = 2.5);

    std::optional<order_event> on_market(const market_event& mkt) override;

    void set_position_open(const std::string& symbol, bool open) override;
    void set_account_equity(double equity) override { equity_ = equity; }

    std::vector<truetest::exits::exit_intent> take_pending_exit_intents() override;

    void on_fill(const fill_event& fill, std::uint64_t opener_order_id) override;

    std::vector<param_def> get_param_schema() const override;

    void set_param(const std::string& key, double value) override;

    std::vector<std::pair<std::string, double>> get_indicator_values(
        const std::string& symbol) const override;

private:
    // Config
    double equity_;
    double risk_fraction_;
    double entry_fee_rate_ = 0.0;
    double exit_fee_rate_  = 0.0;
    double entry_slip_bps_ = 0.0;
    double exit_slip_bps_  = 0.0;
    double fixed_fee_per_leg_ = 0.0;
    double max_notional_frac_ = 0.0;
    std::size_t atr_period_;
    std::size_t vol_period_;
    std::size_t lookback_;
    double breakout_threshold_;   // e.g. 0.0075 = 0.75%
    double atr_expansion_;        // 0.15 = 15%
    double vol_mult_;             // 1.8x
    double min_rr_;               // 2.5

    // Per-symbol state
    struct SymbolState
    {
        average_true_range atr{14};
        simple_moving_average vol_sma{20};

        std::deque<double> highs;
        std::deque<double> lows;
        std::deque<double> atr_history;   // recent ATRs for min/low detection
        std::deque<double> vol_history;   // for surge confirmation

        bool position_open = false;
        int open_lots = 0;                // for multi-lot if needed

        // Simple breakout state machine
        enum class Phase { SCANNING, CONSOLIDATING, BROKEN, RETESTED };
        Phase phase = Phase::SCANNING;
        double breakout_level = 0.0;
        int bars_since_break = 0;
        double consolidation_low = 0.0;
        double consolidation_high = 0.0;
        double atr_at_break = 0.0;
    };

    SymbolStateStore<SymbolState> states_;
    std::vector<truetest::exits::exit_intent> pending_intents_;

    // Helpers
    double compute_quantity(double price, double sl_distance) const;
    bool detect_consolidation(const SymbolState& st, double& out_high, double& out_low) const;
    bool check_breakout_gates(const SymbolState& st, double open, double close, double high, double low,
                              double atr, double vol, double prior_atr_low,
                              double prior_vol_avg,
                              double& out_break_level) const;
    void trim_deques(SymbolState& st);
    double get_recent_atr_min(const SymbolState& st, std::size_t n = 10) const;
};
