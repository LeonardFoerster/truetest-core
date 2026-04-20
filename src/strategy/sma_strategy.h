#pragma once
#include "../core/event.h"
#include "../indicator/sma.h"
#include "strategy_interface.h"

#include <optional>
#include <string>
#include <unordered_map>

class sma_strategy : public IStrategy
{
public:
    explicit sma_strategy(std::size_t period = 20);
    std::optional<order_event> on_market(const market_event& mkt) override;
    void set_position_open(const std::string& symbol, bool open) override;

    std::vector<param_def> get_param_schema() const override
    {
        return {
            {"period", static_cast<double>(period_), 1, 10000, "SMA lookback period"},
        };
    }

    void set_param(const std::string& key, double value) override
    {
        if (key == "period") { period_ = static_cast<std::size_t>(value); smas_.clear(); }
        else throw std::runtime_error("Unknown parameter: " + key);
    }

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
    std::unordered_map<std::string, simple_moving_average> smas_;
    std::unordered_map<std::string, bool> position_open_;

    simple_moving_average& get_sma(const std::string& symbol);
};
