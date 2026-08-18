#pragma once
#include "../core/event.h"
#include "../indicator/sma.h"
#include "exits/exit_intent.h"
#include "strategy_interface.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Bar SMA strategy: signal on close, default fill is market (pairs with
// engine execution_bar_delay=1 → next-bar open). fill_style=1 restores
// legacy LIMIT@close. Optimistic position_open_ on emit stops free-fire
// while orders are delayed or resting (engine resyncs on reject/fill).
//
// Pure recipe: sizing via equity * risk_fraction against platform SL distance;
// protective SL/TP via exit_intent (defaults 0.3% / 1%).
class sma_strategy : public IStrategy
{
public:
    explicit sma_strategy(std::size_t period = 20);
    std::optional<order_event> on_market(const market_event& mkt) override;
    void set_position_open(const std::string& symbol, bool open) override;
    void set_account_equity(double equity) override { equity_ = equity; }
    std::vector<truetest::exits::exit_intent> take_pending_exit_intents() override;

    std::vector<param_def> get_param_schema() const override
    {
        return {
            {"period", static_cast<double>(period_), 1, 10000, "SMA lookback period"},
            {"fill_style", static_cast<double>(fill_style_), 0, 1,
             "0=market (default, next-open with bar delay); 1=limit_at_close"},
            {"equity", equity_, 0, 1e18, "Account equity for position sizing"},
            {"risk_fraction", risk_fraction_, 0, 1, "Fraction of equity risked per trade"},
            {"sl_pct", sl_pct_, 0, 1, "Stop-loss as fraction of entry"},
            {"tp_pct", tp_pct_, 0, 1, "Take-profit as fraction of entry"},
            {"entry_fee_rate", entry_fee_rate_, 0, 0.05, "Entry fee as fraction of notional"},
            {"exit_fee_rate", exit_fee_rate_, 0, 0.05, "Exit fee as fraction of notional"},
        };
    }

    void set_param(const std::string& key, double value) override
    {
        if (key == "period") { period_ = static_cast<std::size_t>(value); smas_.clear(); }
        else if (key == "fill_style") {
            const int v = static_cast<int>(value);
            if (v < 0 || v > 1) throw std::runtime_error("fill_style must be 0 or 1");
            fill_style_ = v;
        }
        else if (key == "equity") equity_ = value;
        else if (key == "risk_fraction") risk_fraction_ = value;
        else if (key == "sl_pct") sl_pct_ = value;
        else if (key == "tp_pct") tp_pct_ = value;
        else if (key == "entry_fee_rate") entry_fee_rate_ = value;
        else if (key == "exit_fee_rate") exit_fee_rate_ = value;
        else throw std::runtime_error("Unknown parameter: " + key);
    }

    void on_fill(const fill_event& fill, std::uint64_t /*opener_order_id*/) override
    {
        // Reconcile exit size with actual filled qty (FR-08 partials).
        const auto& sym = fill.get_symbol();
        const double filled = fill.get_filled_quantity();
        if (fill.get_side() == order_side::buy)
        {
            opener_filled_[sym] += filled;
            position_qty_[sym] = opener_filled_[sym];
        }
        else
        {
            position_qty_[sym] = std::max(0.0, position_qty_[sym] - filled);
            if (position_qty_[sym] <= 1e-12)
                opener_filled_[sym] = 0.0;
        }
    }

    // MC / reuse: clear indicator + position maps so trials do not leak state.
    void reset(uint64_t /*seed*/ = 0) override
    {
        smas_.clear();
        position_open_.clear();
        position_qty_.clear();
        opener_filled_.clear();
        pending_intents_.clear();
    }
    bool supports_mc_trial_reuse() const override { return true; }

    std::vector<std::pair<std::string, double>> get_indicator_values(
        const std::string& symbol) const override
    {
        std::vector<std::pair<std::string, double>> vals;
        auto it = smas_.find(symbol);
        if (it != smas_.end() && it->second.ready())
        {
            vals.emplace_back("sma_" + std::to_string(period_), it->second.value());
        }
        return vals;
    }

private:
    std::size_t period_;
    int fill_style_ = 0; // 0=market, 1=limit_at_close
    double equity_ = 10000.0;
    double risk_fraction_ = 0.02;
    double sl_pct_ = 0.003;
    double tp_pct_ = 0.01;
    double entry_fee_rate_ = 0.0;
    double exit_fee_rate_ = 0.0;
    std::unordered_map<std::string, simple_moving_average> smas_;
    std::unordered_map<std::string, bool> position_open_;
    std::unordered_map<std::string, double> position_qty_;
    std::unordered_map<std::string, double> opener_filled_;
    std::vector<truetest::exits::exit_intent> pending_intents_;

    simple_moving_average& get_sma(const std::string& symbol);
    double size_long(double entry) const;

    order_type order_type_for_fill_style() const
    {
        return fill_style_ == 1 ? order_type::limit : order_type::market;
    }
};
