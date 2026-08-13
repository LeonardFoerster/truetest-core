#pragma once
#include "../core/event.h"
#include "../indicator/atr.h"
#include "../indicator/sma.h"
#include "../indicator/swing_detector.h"
#include "exits/exit_intent.h"
#include "strategy_interface.h"
#include "symbol_state_store.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class mean_reversion_strategy : public IStrategy
{
public:
    explicit mean_reversion_strategy(std::size_t period = 20,
                                     double equity = 10000.0,
                                     double risk_fraction = 0.02,
                                     double sl_pct = 0.003,
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

    void reset(uint64_t seed = 0) override;
    bool supports_mc_trial_reuse() const override { return true; }

private:
    std::size_t period_;
    double equity_;
    double risk_fraction_;
    double sl_pct_;
    double tp_pct_;

    double entry_fee_rate_ = 0.0;
    double exit_fee_rate_  = 0.0;
    double entry_slip_bps_ = 0.0;
    double exit_slip_bps_  = 0.0;
    double fixed_fee_per_leg_ = 0.0;
    double max_notional_frac_ = 0.0;

    std::size_t atr_period_ = 14;
    double sl_atr_mult_ = 1.5;
    double tp_atr_mult_ = 3.0;

    std::size_t swing_strength_ = 2;
    std::size_t swing_history_  = 32;
    double fib_sl_retracement_  = 0.618;
    double fib_tp_extension_    = 1.618;
    double atr_buffer_mult_sl_  = 0.20;
    double atr_buffer_mult_tp_  = 0.10;
    std::string exit_style_     = "fib"; // "pct" | "atr" | "fib"

    double scale_out_ratio_     = 0.5;
    double trail_atr_mult_      = 2.0;

    enum class sma_side : int8_t { unknown = 0, below = -1, equal = 2, above = 1 };

    struct symbol_gate
    {
        bool     position_open = false;
        sma_side prev_side     = sma_side::unknown;
    };

    // Dense id-indexed state (see SymbolStateStore) — no string-hash per bar.
    struct symbol_state
    {
        simple_moving_average sma;
        average_true_range    atr;
        swing_detector        swing;
        symbol_gate           gate;

        symbol_state(std::size_t sma_period, std::size_t atr_period,
                     std::size_t swing_strength, std::size_t swing_history)
            : sma(sma_period)
            , atr(atr_period)
            , swing(swing_strength, swing_history)
        {}
    };

    SymbolStateStore<symbol_state> states_;
    std::vector<truetest::exits::exit_intent> pending_intents_;

    bool needs_atr() const { return exit_style_ != "pct"; }
    bool needs_swing() const { return exit_style_ == "fib"; }

    double compute_quantity_with_sl(double entry, double sl_price, bool is_long) const;

    std::vector<truetest::exits::exit_intent>
    create_exit_intents(symbol_state& st, const std::string& symbol,
                        double entry, double qty, bool is_long);

    double compute_intended_sl(symbol_state& st, double entry, bool is_long) const;

    std::optional<order_event> try_entry(symbol_state& st,
                                         const std::string& symbol,
                                         std::chrono::system_clock::time_point ts,
                                         double price, double sma_value);

    void update_indicators(symbol_state& st, double high, double low, double close);

    static sma_side side_of(double price, double sma_value);

    bool use_fib_exits_ = false;
};
