#pragma once
#include "../core/event.h"
#include "../indicator/atr.h"
#include "../indicator/sma.h"
#include "../indicator/swing_detector.h"
#include "exits/exit_intent.h"
#include "strategy_interface.h"

#include <optional>
#include <string>
#include <unordered_map>

class mean_reversion_strategy : public IStrategy
{
public:
    explicit mean_reversion_strategy(std::size_t period = 20,
                                     double equity = 10000.0,
                                     double risk_fraction = 0.02,
                                     double sl_pct = 0.005,
                                     double tp_pct = 0.01,
                                     std::size_t atr_period = 14);
    std::optional<order_event> on_market(const market_event& mkt) override;
    std::optional<order_event> on_tick(const tick_event& te) override;
    void set_position_open(const std::string& symbol, bool open) override;

    std::vector<truetest::exits::exit_intent> take_pending_exit_intents() override;

    void update_equity(double equity) { equity_ = equity; }

    std::vector<param_def> get_param_schema() const override;
    void set_param(const std::string& key, double value) override;
    std::vector<std::pair<std::string, double>> get_indicator_values(
        const std::string& symbol) const override;

private:
    std::size_t period_;
    double equity_;
    double risk_fraction_;
    double sl_pct_;
    double tp_pct_;

    // ATR support (already partially wired)
    std::size_t atr_period_ = 14;
    double sl_atr_mult_ = 1.5;
    double tp_atr_mult_ = 3.0;

    // Phase 1: Swing + Fibonacci exits
    std::size_t swing_strength_ = 2;
    std::size_t swing_history_  = 32;
    double fib_sl_retracement_  = 0.618;
    double fib_tp_extension_    = 1.618;
    double atr_buffer_mult_sl_  = 0.20;
    double atr_buffer_mult_tp_  = 0.10;
    bool   use_fib_exits_       = true;

    // Phase 2: Scale-out + trailing
    double scale_out_ratio_     = 0.5;   // fraction taken at first Fib TP (rest is runner)
    double trail_atr_mult_      = 2.0;   // trailing stop distance in ATR for runner

    std::unordered_map<std::string, simple_moving_average> smas_;
    std::unordered_map<std::string, average_true_range>    atrs_;
    std::unordered_map<std::string, swing_detector>        swings_;

    std::vector<truetest::exits::exit_intent> pending_intents_;

    simple_moving_average& get_sma(const std::string& symbol);
    average_true_range&    get_atr(const std::string& symbol);
    swing_detector&        get_swing(const std::string& symbol);

    double compute_quantity(double price) const;

    std::vector<truetest::exits::exit_intent>
    create_exit_intents(const std::string& symbol, double entry, double qty, bool is_long);

    void reset(uint64_t seed = 0) override;
};
