#pragma once

// Fee- and slippage-aware position sizing.
//
// Fixed-risk without costs systematically overshoots the risk budget:
//   L_actual ≈ q * |entry_fill - exit_fill| + fee_entry + fee_exit
// Strategies size with intended mid/close; market fills and stop exits
// are adverse. This helper folds proportional fees, fixed fees, and
// adverse slip estimates into the per-unit risk so
//   q * risk_per_unit ≈ equity * risk_fraction.

#include <algorithm>
#include <cmath>

namespace truetest::risk {

struct risk_size_inputs
{
    double equity = 0.0;
    double risk_fraction = 0.0;   // fraction of equity at risk if stopped
    double entry_price = 0.0;     // intended mid / close
    double stop_price = 0.0;      // absolute stop-loss price
    bool   is_long = true;

    // Proportional fees as fraction of notional (e.g. 0.0004 = 4 bps).
    double entry_fee_rate = 0.0;
    double exit_fee_rate = 0.0;

    // Adverse slippage in bps applied to entry and exit legs.
    double entry_slip_bps = 0.0;
    double exit_slip_bps = 0.0;

    // Fixed fee charged once per leg (FixedFeeModel). Round-trip = 2x.
    double fixed_fee_per_leg = 0.0;

    // Optional hard cap on position notional as fraction of equity (0 = off).
    // Independent of risk_fraction — do not reuse risk_fraction for this.
    double max_notional_frac = 0.0;

    // Optional hard cap on max dollar loss per trade (e.g. from RiskManager limits).
    double max_loss_per_trade_cap = 0.0;
};

// Adverse fill estimates (long buy lifts; short sell dumps).
inline double adverse_entry_price(double intended, bool is_long, double slip_bps)
{
    if (!(intended > 0.0)) return 0.0;
    const double m = std::max(0.0, slip_bps) * 1.0e-4;
    return is_long ? intended * (1.0 + m) : intended * (1.0 - m);
}

// Long stop exits with a sell (worse = lower); short covers with a buy (worse = higher).
inline double adverse_exit_price(double stop, bool is_long, double slip_bps)
{
    if (!(stop > 0.0)) return 0.0;
    const double m = std::max(0.0, slip_bps) * 1.0e-4;
    return is_long ? stop * (1.0 - m) : stop * (1.0 + m);
}

// Gross + fee risk per unit qty at the estimated fills.
inline double risk_per_unit(const risk_size_inputs& in,
                            double entry_est, double exit_est)
{
    double price_risk = in.is_long ? (entry_est - exit_est) : (exit_est - entry_est);
    if (price_risk <= 1e-12)
    {
        // Degenerate stop: floor at 1% of entry so we never emit infinite size.
        price_risk = 0.01 * std::max(entry_est, 1e-12);
    }
    const double prop_fees =
        entry_est * std::max(0.0, in.entry_fee_rate) +
        exit_est  * std::max(0.0, in.exit_fee_rate);
    return price_risk + prop_fees;
}

// Fixed-risk quantity: risk_budget ≈ q * risk_per_unit + 2 * fixed_fee.
inline double compute_risk_quantity(const risk_size_inputs& in)
{
    if (!(in.equity > 0.0) || !(in.risk_fraction > 0.0) || !(in.entry_price > 0.0))
        return 0.0;
    if (!(in.stop_price > 0.0))
        return 0.0;

    const double entry_est =
        adverse_entry_price(in.entry_price, in.is_long, in.entry_slip_bps);
    if (!(entry_est > 0.0))
        return 0.0;

    const double exit_est =
        adverse_exit_price(in.stop_price, in.is_long, in.exit_slip_bps);
    if (!(exit_est > 0.0))
        return 0.0;

    const double denom = risk_per_unit(in, entry_est, exit_est);
    if (!(denom > 1e-12))
        return 0.0;

    double risk_budget = in.equity * in.risk_fraction;
    if (in.max_loss_per_trade_cap > 0.0)
    {
        const double max_allowed = in.max_loss_per_trade_cap * 0.95;
        if (risk_budget > max_allowed)
            risk_budget = max_allowed;
    }
    const double fixed_round_trip = 2.0 * std::max(0.0, in.fixed_fee_per_leg);
    if (risk_budget <= fixed_round_trip)
        return 0.0;

    double qty = (risk_budget - fixed_round_trip) / denom;

    if (in.max_notional_frac > 0.0)
    {
        const double max_qty =
            (in.equity * in.max_notional_frac) /
            (entry_est * (1.0 + std::max(0.0, in.entry_fee_rate)));
        qty = std::min(qty, max_qty);
    }

    return qty > 0.0 ? qty : 0.0;
}

// Notional sizing: deploy risk_fraction of equity as position notional,
// shrinking qty so entry fee + adverse entry slip still fit the budget.
inline double compute_notional_quantity(double equity,
                                        double risk_fraction,
                                        double price,
                                        double entry_fee_rate = 0.0,
                                        double entry_slip_bps = 0.0,
                                        bool is_long = true,
                                        double fixed_fee = 0.0)
{
    if (!(equity > 0.0) || !(risk_fraction > 0.0) || !(price > 0.0))
        return 0.0;

    const double entry_est = adverse_entry_price(price, is_long, entry_slip_bps);
    if (!(entry_est > 0.0))
        return 0.0;

    const double budget = equity * risk_fraction;
    if (budget <= fixed_fee)
        return 0.0;

    // cash_out ≈ q * entry_est * (1 + fee_rate) + fixed_fee  (long buy)
    // For shorts the "notional" target is the same magnitude.
    const double unit_cost = entry_est * (1.0 + std::max(0.0, entry_fee_rate));
    if (!(unit_cost > 1e-12))
        return 0.0;

    const double qty = (budget - fixed_fee) / unit_cost;
    return qty > 0.0 ? qty : 0.0;
}

// Diagnostic: expected cash loss if stopped at designed SL with costs.
inline double estimate_stop_loss(const risk_size_inputs& in, double qty)
{
    if (!(qty > 0.0) || !(in.entry_price > 0.0) || !(in.stop_price > 0.0))
        return 0.0;

    const double entry_est =
        adverse_entry_price(in.entry_price, in.is_long, in.entry_slip_bps);
    const double exit_est =
        adverse_exit_price(in.stop_price, in.is_long, in.exit_slip_bps);
    const double per_unit = risk_per_unit(in, entry_est, exit_est);
    return qty * per_unit + 2.0 * std::max(0.0, in.fixed_fee_per_leg);
}

} // namespace truetest::risk
