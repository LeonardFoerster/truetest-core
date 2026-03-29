#pragma once

#include "strategy_interface.h"
#include "mean_reversion_strategy.h"
#include "sma_strategy.h"
#include "ma_crossover_strategy.h"

#include <memory>
#include <string>
#include <vector>

struct strategy_params {
    std::size_t sma_period = 20;
    double balance = 10000.0;
    double risk_fraction = 0.02;
    double sl_pct = 0.005;
    double tp_pct = 0.01;
};

class StrategyFactory {
public:
    static std::shared_ptr<IStrategy> create(
        const std::string& name, const strategy_params& params = {})
    {
        if (name == "sma")
            return std::make_shared<sma_strategy>(params.sma_period);
        if (name == "ma-crossover")
            return std::make_shared<ma_crossover_strategy>(params.sma_period);
        // Default: mean-reversion
        return std::make_shared<mean_reversion_strategy>(
            params.sma_period, params.balance, params.risk_fraction,
            params.sl_pct, params.tp_pct);
    }

    static std::vector<std::string> available() {
        return {"mean-reversion", "sma", "ma-crossover"};
    }
};
