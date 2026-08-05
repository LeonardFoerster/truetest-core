#pragma once
#include "../core/event.h"
#include "../indicator/atr.h"
#include "../indicator/rolling_extreme.h"
#include "../indicator/sma.h"
#include "exits/exit_intent.h"
#include "strategy_interface.h"
#include "symbol_state_store.h"

#include <optional>
#include <string>
#include <vector>

// Larry Connors style long-only swing strategy.
//
// Python reference:
//   MA200    = close.rolling(200).mean()   # regime filter
//   7D_Low   = close.rolling(7).min()      # entry trigger
//   7D_High  = close.rolling(7).max()      # exit trigger
//   ATR      = wilders_atr(high, low, close)
//
//   LongEntry = (close > MA200) & (close == 7D_Low)
//   LongExit  = (close == 7D_High)
//
// Buy when the regime is bullish (close above the 200-bar SMA) and price prints
// a fresh `entry_period`-bar low; exit the long when price prints a fresh
// `exit_period`-bar high. Also arms platform SL/TP (default 0.3% / 1%).
// ATR is computed for diagnostics / sizing and exposed via get_indicator_values().
//
// `close == rolling.min()` is implemented as `close <= rolling.min()`: the
// rolling minimum already includes the current bar, so it can never exceed the
// current close — the inequality is therefore exactly the equality test, but
// robust to floating-point round-trips. Likewise `close >= rolling.max()`.
class larry_connor_strategy : public IStrategy
{
public:
    explicit larry_connor_strategy(std::size_t ma_period = 200,
                                   std::size_t entry_period = 7,
                                   std::size_t exit_period = 7,
                                   std::size_t atr_period = 14,
                                   double equity = 10000.0,
                                   double risk_fraction = 0.02);

    std::optional<order_event> on_market(const market_event& mkt) override;
    void set_position_open(const std::string& symbol, bool open) override;
    std::vector<truetest::exits::exit_intent> take_pending_exit_intents() override;

    void update_equity(double equity) { equity_ = equity; }

    std::vector<param_def> get_param_schema() const override;
    void set_param(const std::string& key, double value) override;
    std::vector<std::pair<std::string, double>> get_indicator_values(
        const std::string& symbol) const override;

    void reset(uint64_t seed = 0) override;

private:
    struct SymbolState
    {
        simple_moving_average ma;
        rolling_extreme       entry_low;
        rolling_extreme       exit_high;
        average_true_range    atr;
        bool                  position_open = false;
        double                open_qty = 0.0;

        SymbolState(std::size_t ma_p, std::size_t entry_p,
                    std::size_t exit_p, std::size_t atr_p)
            : ma(ma_p), entry_low(entry_p), exit_high(exit_p), atr(atr_p) {}
    };

    double compute_quantity(double price) const;

    std::size_t ma_period_;
    std::size_t entry_period_;
    std::size_t exit_period_;
    std::size_t atr_period_;
    double      equity_;
    double      risk_fraction_;
    double      sl_pct_ = 0.003;
    double      tp_pct_ = 0.01;
    double      entry_fee_rate_ = 0.0;
    double      entry_slip_bps_ = 0.0;
    double      fixed_fee_per_leg_ = 0.0;

    std::vector<truetest::exits::exit_intent> pending_intents_;
    SymbolStateStore<SymbolState> states_;
};
