#pragma once

#include "strategy_interface.h"
#include "strategy_registry.h"

#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <vector>

// F-10 (docs/todos/11-F-forensic-lifecycle-audit.md): this used to be a
// second, hardcoded strategy table living next to StrategyRegistry. The
// registry is what actually validates and constructs `--strategy`, so the
// table here drifted: it listed six strategies, omitted the ones registered
// later (ema-rsi-atr-pullback among them), and silently fell back to
// mean-reversion for any name it did not recognise — so a typo produced a
// different strategy rather than an error.
//
// It is now a thin adapter over StrategyRegistry. One registry, one list,
// one construction path. `strategy_params` is preserved as a convenience:
// values are applied through the strategy's own parameter schema, so a
// strategy that does not expose a knob simply keeps its default.

struct strategy_params {
    std::size_t sma_period = 20;
    double balance = 10000.0;
    double risk_fraction = 0.02;
    double sl_pct = 0.003;
    double tp_pct = 0.01;
};

class StrategyFactory {
public:
    static std::shared_ptr<IStrategy> create(
        const std::string& name, const strategy_params& params = {})
    {
        auto strategy = StrategyRegistry::instance().create(name);
        if (strategy)
            apply_params(*strategy, params);
        return strategy;
    }

    static bool has(const std::string& name)
    {
        return StrategyRegistry::instance().has(name);
    }

    static std::vector<std::string> available()
    {
        return StrategyRegistry::instance().available();
    }

private:
    // Schema-driven so a strategy that does not declare a knob is left
    // alone rather than being handed a constructor argument it never had.
    static void apply_params(IStrategy& strategy, const strategy_params& params)
    {
        const auto schema = strategy.get_param_schema();
        auto set_if = [&](const char* param, double value) {
            for (const auto& p : schema)
            {
                if (p.name != param) continue;
                try { strategy.set_param(param, value); }
                catch (const std::exception&) { /* out of range for this strategy */ }
                return;
            }
        };
        set_if("period", static_cast<double>(params.sma_period));
        set_if("sma_period", static_cast<double>(params.sma_period));
        set_if("balance", params.balance);
        set_if("equity", params.balance);
        set_if("risk_fraction", params.risk_fraction);
        set_if("sl_pct", params.sl_pct);
        set_if("tp_pct", params.tp_pct);
    }
};
