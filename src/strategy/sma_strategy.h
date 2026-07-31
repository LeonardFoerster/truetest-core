#pragma once
#include "../core/event.h"
#include "../indicator/sma.h"
#include "strategy_interface.h"

#include <optional>
#include <string>
#include <unordered_map>

// Bar SMA strategy: signal on close, default fill is market (pairs with
// engine execution_bar_delay=1 → next-bar open). fill_style=1 restores
// legacy LIMIT@close. Optimistic position_open_ on emit stops free-fire
// while orders are delayed or resting (engine resyncs on reject/fill).
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
            {"fill_style", static_cast<double>(fill_style_), 0, 1,
             "0=market (default, next-open with bar delay); 1=limit_at_close"},
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
    int fill_style_ = 0; // 0=market, 1=limit_at_close
    std::unordered_map<std::string, simple_moving_average> smas_;
    std::unordered_map<std::string, bool> position_open_;

    simple_moving_average& get_sma(const std::string& symbol);

    order_type order_type_for_fill_style() const
    {
        return fill_style_ == 1 ? order_type::limit : order_type::market;
    }
};
