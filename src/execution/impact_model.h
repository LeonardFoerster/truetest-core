#pragma once

#include "../core/event.h"

#include <cmath>

// Applies slippage to aggressive orders. BUY: raises price (worse); SELL:
// lowers it. reference_px is typically mid or the intended price.
class IImpactModel
{
public:
    virtual ~IImpactModel() = default;
    virtual double effective_price(order_side side,
                                   double qty,
                                   double reference_px) const = 0;
};

class ZeroImpactModel : public IImpactModel
{
public:
    double effective_price(order_side /*side*/, double /*qty*/, double reference_px) const override
    {
        return reference_px;
    }
};

// impact_bps = k * sqrt(qty/adv). Typical crypto k ~ O(10) bps for qty
// ≈ 1% of ADV. adv=0 disables (returns reference). Coarse model — real
// Almgren-Chriss is more, but this captures sub-linear impact growth.
class SquareRootImpactModel : public IImpactModel
{
public:
    SquareRootImpactModel(double k_bps, double adv)
        : k_bps_(k_bps), adv_(adv) {}

    double effective_price(order_side side, double qty, double reference_px) const override
    {
        if (!(adv_ > 0.0) || !(qty > 0.0) || !(reference_px > 0.0))
            return reference_px;

        const double impact_bps = k_bps_ * std::sqrt(qty / adv_);
        const double impact_frac = impact_bps / 1.0e4;
        const double sign = (side == order_side::buy) ? +1.0 : -1.0;
        return reference_px * (1.0 + sign * impact_frac);
    }

private:
    double k_bps_;
    double adv_;
};
