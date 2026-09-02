#pragma once

#include "strategy_interface.h"

#include <string_view>

// Shared CLI/MC cost → strategy sizing injection.
// Prefer taker for market-style risk; fall back to maker. Fixed fee and
// slippage apply only when the strategy schema exposes them.
//
// F-04 (docs/todos/11-F-forensic-lifecycle-audit.md): the slippage estimate
// used to come solely from `bar_spread_bps` — the `--bar-spread-bps` flag,
// which the CLI itself documents as "DEPRECATED, no effect". It defaults to
// zero, so position sizing was blind to the only execution cost that exists
// and overshot by ≈4.9×: on the audited run the strategy sized 25.80 units
// for a $200 risk budget and lost an average of $776.80 per trade.
//
// The parameter that genuinely governs fill prices is the synthetic book's
// half spread (`--mm-spread-pct`, MarketMaker::mm_calibration::base_spread_pct):
// a market entry crosses to the first resting level at mid * (1 ± half),
// so the half spread *is* the entry slip. The vol-widening term
// (vol * vol_spread_mult, capped at max_half_spread_pct) only widens it
// further at runtime, so the base is a lower bound and the honest estimate
// available at sizing time.
//
// `bar_spread_bps` is retained only so an explicit larger operator estimate
// still wins; it contributes nothing by default and the CLI warns about it.
inline void apply_execution_cost_params(IStrategy& strategy,
                                        double maker_rate,
                                        double taker_rate,
                                        double bar_spread_bps,
                                        std::string_view fee_model = {},
                                        double fee_value = 0.0,
                                        double mm_half_spread_pct = 0.0)
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

    // `zero` is an explicit research opt-out. Do not retain the default
    // proportional rates in sizing while the execution model charges none.
    // FixedFeeModel charges a cash amount only; do not size it as though the
    // default tiered percentage also applied.
    const double entry_fee = (fee_model == "zero" || fee_model == "fixed") ? 0.0
        : (taker_rate > 0.0) ? taker_rate
        : (maker_rate > 0.0) ? maker_rate : 0.0;
    set_if("entry_fee_rate", entry_fee);
    set_if("exit_fee_rate", entry_fee);

    if (fee_model == "fixed")
        set_if("fixed_fee_per_leg", fee_value);

    // The book's half spread is already one-sided: an entry pays it once on
    // the way in and once on the way out. bar_spread_bps is a full spread,
    // hence the halving, and only participates when an operator set it.
    const double model_slip_bps = (mm_half_spread_pct > 0.0)
        ? mm_half_spread_pct * 10000.0 : 0.0;
    const double legacy_slip_bps = (bar_spread_bps > 0.0)
        ? bar_spread_bps * 0.5 : 0.0;
    const double slip_bps = (legacy_slip_bps > model_slip_bps)
        ? legacy_slip_bps : model_slip_bps;

    if (slip_bps > 0.0)
    {
        set_if("entry_slip_bps", slip_bps);
        set_if("exit_slip_bps", slip_bps);
    }
}
