#pragma once

#include "strategy_interface.h"
#include "indicator/ema.h"
#include "indicator/stochastic.h"
#include "indicator/swing_detector.h"
#include "indicator/atr.h"
#include "indicator/ema_regime.h"

#include "exits/exit_intent.h"

#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * Structure Continuation Strategy (EMA 50/100 + Stochastic(5,3,3) + Swing Structure).
 *
 * Implements the user's trend-continuation rules with market structure filters:
 * - Only trade in direction of confirmed HH (up) or LL (down) structure.
 * - After Seitwärtsphase: wait for first valid confluence signal (orientation only),
 *   then trade the *next* valid signal as trend continuation.
 * - EMA(100) as dynamic support/resistance and SL anchor.
 * - Stochastic for timing + divergence.
 * - Y-förmig expansion and sideways regime from EMA Regime helper.
 *
 * Phase 2 skeleton:
 * - Long + Short symmetric from the start.
 * - Temporary position sizing (risk-based or nominal). Marked as "later replace by proper risk layer".
 * - Full reset() support for Monte Carlo.
 * - Rich get_indicator_values() for TUI / analytics / MC reporting.
 */
class structure_continuation_strategy : public IStrategy
{
public:
    // Default constructor for factory / registry
    structure_continuation_strategy();

    // Full constructor (used for tests / advanced usage)
    explicit structure_continuation_strategy(
        double risk_fraction,
        std::size_t swing_strength = 2,
        std::size_t swing_history  = 32);

    std::optional<order_event> on_market(const market_event& mkt) override;

    void set_position_open(const std::string& symbol, bool open) override;

    std::vector<truetest::exits::exit_intent> take_pending_exit_intents() override;

    void on_fill(const fill_event& fill, std::uint64_t opener_order_id) override;

    std::vector<param_def> get_param_schema() const override;
    void set_param(const std::string& key, double value) override;

    std::vector<std::pair<std::string, double>> get_indicator_values(
        const std::string& symbol) const override;

    void reset(uint64_t seed = 0) override;

private:
    // Config (temporary sizing parameters)
    double      risk_fraction_;
    std::size_t swing_strength_;
    std::size_t swing_history_;

    // Per-symbol state
    struct SymbolState
    {
        // Indicators
        exponential_moving_average ema50{50};
        exponential_moving_average ema100{100};
        stochastic_oscillator      stoch{5, 3, 3};
        swing_detector             swing{2, 32};
        average_true_range         atr{14};
        ema_regime_detector        regime{14, 48, 1.65, 2.8, 1.9};

        // Continuation Phase FSM (core user rule)
        enum class ContinuationPhase : uint8_t {
            NORMAL = 0,              // default scanning
            AFTER_SIDEWAYS,          // just exited sideways
            ORIENTATION_PENDING,     // consumed first post-sideways confluence (bias only)
            READY_FOR_CONTINUATION,  // next valid confluence = tradable
            IN_TRADE,
            COOLDOWN
        };
        ContinuationPhase phase = ContinuationPhase::NORMAL;

        // Orientation bias from the first post-sideways signal
        int orientation_bias = 0;   // +1 long, -1 short, 0 none

        int bars_since_sideways_exit = 0;
        int signals_since_orientation = 0;

        // Position tracking (supplements engine portfolio)
        bool position_open = false;
        int  open_lots     = 0;
        std::uint64_t last_opener_id = 0;

        // Last known values for convenience
        double last_close = 0.0;
    };

    std::unordered_map<std::string, SymbolState> states_;

    // Strategy-level pending exit intents (populated by create_exit_intents at entry).
    // We use a flat list (breakout pattern) for clean hand-off via take_pending_exit_intents.
    // This keeps things simple for v1 (one lot per symbol).
    std::vector<truetest::exits::exit_intent> pending_intents_;

    SymbolState& get_state(const std::string& symbol);

    // Core logic
    void update_all_indicators(SymbolState& st, double open, double high, double low, double close);
    bool is_valid_long_setup(const SymbolState& st) const;
    bool is_valid_short_setup(const SymbolState& st) const;

    void advance_continuation_fsm(SymbolState& st, bool long_signal, bool short_signal);

    // Temporary sizing (clearly marked for later replacement)
    double compute_quantity(double price, double sl_distance, double equity) const;

    // Exit intent creation (uses ExitManager) — implements user's SL/TP rules
    void create_exit_intents(const std::string& symbol, SymbolState& st,
                             double entry_price, double qty, bool is_long);
};
