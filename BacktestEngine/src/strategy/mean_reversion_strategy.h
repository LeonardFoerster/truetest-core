#pragma once
#include "../core/event.h"
#include "../indicator/sma.h"
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
                                     double tp_pct = 0.01);
    std::optional<order_event> on_market(const market_event& mkt) override;
    std::optional<order_event> on_tick(const tick_event& te) override;
    void set_position_open(const std::string& symbol, bool open) override;

    void update_equity(double equity) { equity_ = equity; }

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
    double equity_;
    double risk_fraction_;
    double sl_pct_;   // stop loss as fraction of entry price (e.g. 0.005 = 0.5%)
    double tp_pct_;   // take profit as fraction of entry price (e.g. 0.01 = 1%)
    std::unordered_map<std::string, simple_moving_average> smas_;
    std::unordered_map<std::string, bool> position_open_;

    simple_moving_average& get_sma(const std::string& symbol);
    double compute_quantity(double price) const;
};
