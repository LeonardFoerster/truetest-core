#pragma once

#include "strategy_interface.h"

#include <string_view>

// Shared CLI/MC cost → strategy sizing injection.
// Prefer taker for market-style risk; fall back to maker. Fixed fee and
// bar-spread half-slip apply only when the strategy schema exposes them.
inline void apply_execution_cost_params(IStrategy& strategy,
                                        double maker_rate,
                                        double taker_rate,
                                        double bar_spread_bps,
                                        std::string_view fee_model = {},
                                        double fee_value = 0.0)
{
    auto schema = strategy.get_param_schema();
    auto has = [&](const char* name) {
        for (const auto& p : schema)
            if (p.name == name) return true;
        return false;
    };
    auto set_if = [&](const char* name, double value) {
        if (value <= 0.0 || !has(name)) return;
        try { strategy.set_param(name, value); }
        catch (const std::exception&) { /* schema race / unknown — ignore */ }
    };

    const double entry_fee = (taker_rate > 0.0) ? taker_rate
                           : (maker_rate > 0.0) ? maker_rate : 0.0;
    set_if("entry_fee_rate", entry_fee);
    set_if("exit_fee_rate", entry_fee);

    if (fee_model == "fixed")
        set_if("fixed_fee_per_leg", fee_value);

    if (bar_spread_bps > 0.0)
    {
        const double half = bar_spread_bps * 0.5;
        set_if("entry_slip_bps", half);
        set_if("exit_slip_bps", half);
    }
}
