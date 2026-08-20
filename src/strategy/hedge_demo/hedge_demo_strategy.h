#pragma once
#include "../../core/event.h"
#include "../../indicator/sma.h"
#include "exits/exit_intent.h"
#include "../strategy_interface.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Demonstrates multi-position support (plan B):
//   - Opens a long leg with its own SL/TP bracket.
//   - After `hedge_gap` bars, opens a short leg on the SAME symbol with
//     its own independent SL/TP bracket. On a spot venue the two legs
//     net at the portfolio level, but lot/attribution state keeps them
//     distinct; exits fire per-lot.
//   - Does NOT use the legacy set_position_open boolean - tracks its own
//     open lot count per (symbol, side) via on_fill.
class hedge_demo_strategy : public IStrategy
{
public:
    explicit hedge_demo_strategy(std::size_t hedge_gap = 5,
                                 double notional = 100.0,
                                 double sl_pct = 0.003,
                                 double tp_pct = 0.01);

    std::optional<order_event> on_market(const market_event& mkt) override;

    std::vector<truetest::exits::exit_intent> take_pending_exit_intents() override;

    void on_fill(const fill_event& fill, std::uint64_t opener_order_id) override;

    std::vector<param_def> get_param_schema() const override
    {
        return {
            {"hedge_gap", static_cast<double>(hedge_gap_), 1, 10000, "Bars between long and short legs"},
            {"notional",  notional_, 0, 1e18, "Notional size of each leg"},
            {"sl_pct",    sl_pct_,   0, 1, "Stop loss as fraction of entry"},
            {"tp_pct",    tp_pct_,   0, 1, "Take profit as fraction of entry"},
        };
    }

    void set_param(const std::string& key, double value) override
    {
        if      (key == "hedge_gap") hedge_gap_ = static_cast<std::size_t>(value);
        else if (key == "notional")  notional_  = value;
        else if (key == "sl_pct")    sl_pct_    = value;
        else if (key == "tp_pct")    tp_pct_    = value;
        else throw std::runtime_error("Unknown parameter: " + key);
    }

private:
    std::size_t hedge_gap_;
    double notional_;
    double sl_pct_;
    double tp_pct_;

    // Per-symbol bar counter since long leg entered. -1 == haven't entered long yet.
    std::unordered_map<std::string, long> bars_since_long_entry_;

    // Per-symbol open-lot count per side, maintained via on_fill.
    std::unordered_map<std::string, int> long_open_;
    std::unordered_map<std::string, int> short_open_;

    // Remembers which opener_ids are long legs vs short legs, so on_fill
    // on a closer knows which counter to decrement.
    std::unordered_map<std::uint64_t, order_side> opener_sides_;

    std::vector<truetest::exits::exit_intent> pending_intents_;
};
