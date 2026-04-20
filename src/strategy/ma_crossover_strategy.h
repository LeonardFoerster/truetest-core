#pragma once
#include "../core/event.h"
#include "../indicator/sma.h"
#include "strategy_interface.h"

#include <optional>
#include <string>
#include <unordered_map>

class ma_crossover_strategy : public IStrategy
{
public:
    explicit ma_crossover_strategy(std::size_t fast_period = 10, std::size_t slow_period = 50);
    std::optional<order_event> on_market(const market_event& mkt) override;
    void set_position_open(const std::string& symbol, bool open) override;

    std::vector<param_def> get_param_schema() const override
    {
        return {
            {"fast_period", static_cast<double>(fast_period_), 1, 10000, "Fast SMA lookback period"},
            {"slow_period", static_cast<double>(slow_period_), 1, 10000, "Slow SMA lookback period"},
        };
    }

    void set_param(const std::string& key, double value) override
    {
        if (key == "fast_period") { fast_period_ = static_cast<std::size_t>(value); fast_smas_.clear(); }
        else if (key == "slow_period") { slow_period_ = static_cast<std::size_t>(value); slow_smas_.clear(); }
        else throw std::runtime_error("Unknown parameter: " + key);
    }

    std::vector<std::pair<std::string, double>> get_indicator_values(
        const std::string& symbol) const override
    {
        std::vector<std::pair<std::string, double>> vals;
        auto fit = fast_smas_.find(symbol);
        if (fit != fast_smas_.end() && fit->second.ready())
            vals.emplace_back("sma_fast_" + std::to_string(fast_period_), fit->second.value());
        auto sit = slow_smas_.find(symbol);
        if (sit != slow_smas_.end() && sit->second.ready())
            vals.emplace_back("sma_slow_" + std::to_string(slow_period_), sit->second.value());
        return vals;
    }

private:
    std::size_t fast_period_;
    std::size_t slow_period_;
    std::unordered_map<std::string, simple_moving_average> fast_smas_;
    std::unordered_map<std::string, simple_moving_average> slow_smas_;
    std::unordered_map<std::string, bool> position_open_;
    std::unordered_map<std::string, bool> prev_fast_above_;

    simple_moving_average& get_fast_sma(const std::string& symbol);
    simple_moving_average& get_slow_sma(const std::string& symbol);
};
