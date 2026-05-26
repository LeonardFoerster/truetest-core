#pragma once

#include "strategy_interface.h"
#include "mean_reversion_strategy.h"
#include "sma_strategy.h"
#include "ma_crossover_strategy.h"
#include "breakout_strategy.h"
#include "coiled_spring_strategy.h"
#include "adaptive_hybrid_strategy.h"
#include "structure_continuation_strategy.h"

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
        if (name == "breakout" || name == "coiled-spring")
            return std::make_shared<breakout_strategy>(params.balance, 0.005); // 0.5% risk per guide (Coiled Spring)
        if (name == "adaptive-hybrid")
            return std::make_shared<AdaptiveHybridStrategy>(AdaptiveHybridConfig{}); // legacy factory path — registry is preferred
        if (name == "structure-continuation")
            return std::make_shared<structure_continuation_strategy>(0.01, 2, 32); // explicit to ensure definition is linked
        return std::make_shared<mean_reversion_strategy>(
            params.sma_period, params.balance, params.risk_fraction,
            params.sl_pct, params.tp_pct);
    }

    static std::vector<std::string> available() {
        return {"mean-reversion", "sma", "ma-crossover", "breakout", "coiled-spring", "adaptive-hybrid", "structure-continuation"};
    }
};
