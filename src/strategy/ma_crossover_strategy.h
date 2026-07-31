#pragma once
#include "../core/event.h"
#include "../indicator/sma.h"
#include "strategy_interface.h"

#include <optional>
#include <string>
#include <unordered_map>

// Fast/slow SMA crossover: signal on close, default fill is market (pairs
// with engine execution_bar_delay=1 → next-bar open). fill_style=1 restores
// legacy LIMIT@close. Optimistic position_open_ on emit stops multi-cycle
// stacking while entry is delayed/unfilled (engine resyncs on reject/fill).
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
            {"fill_style", static_cast<double>(fill_style_), 0, 1,
             "0=market (default, next-open with bar delay); 1=limit_at_close"},
        };
    }

    void set_param(const std::string& key, double value) override
    {
        if (key == "fast_period") { fast_period_ = static_cast<std::size_t>(value); fast_smas_.clear(); }
        else if (key == "slow_period") { slow_period_ = static_cast<std::size_t>(value); slow_smas_.clear(); }
        else if (key == "fill_style") {
            const int v = static_cast<int>(value);
            if (v < 0 || v > 1) throw std::runtime_error("fill_style must be 0 or 1");
            fill_style_ = v;
        }
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
    int fill_style_ = 0; // 0=market, 1=limit_at_close
    std::unordered_map<std::string, simple_moving_average> fast_smas_;
    std::unordered_map<std::string, simple_moving_average> slow_smas_;
    std::unordered_map<std::string, bool> position_open_;
    std::unordered_map<std::string, bool> prev_fast_above_;

    simple_moving_average& get_fast_sma(const std::string& symbol);
    simple_moving_average& get_slow_sma(const std::string& symbol);

    order_type order_type_for_fill_style() const
    {
        return fill_style_ == 1 ? order_type::limit : order_type::market;
    }
};
