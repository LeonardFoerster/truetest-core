#pragma once

#include "../core/event.h"
#include <algorithm>

class IFeeModel
{
public:
    virtual ~IFeeModel() = default;
    virtual double compute_commission(order_side side, double quantity, double price) const = 0;
};

class ZeroFeeModel : public IFeeModel
{
public:
    double compute_commission(order_side /*side*/, double /*quantity*/, double /*price*/) const override
    {
        return 0.0;
    }
};

class FixedFeeModel : public IFeeModel
{
public:
    explicit FixedFeeModel(double fee_per_trade) : fee_per_trade_(fee_per_trade) {}

    double compute_commission(order_side /*side*/, double /*quantity*/, double /*price*/) const override
    {
        return fee_per_trade_;
    }

private:
    double fee_per_trade_;
};

class TieredFeeModel : public IFeeModel
{
public:
    TieredFeeModel(double maker_rate, double taker_rate)
        : maker_rate_(maker_rate), taker_rate_(taker_rate) {}

    double compute_commission(order_side side, double quantity, double price) const override
    {
        double notional = std::abs(quantity * price);
        // Buys are takers (crossing the spread), sells are makers (providing liquidity)
        double rate = (side == order_side::buy) ? taker_rate_ : maker_rate_;
        return notional * rate;
    }

private:
    double maker_rate_;
    double taker_rate_;
};
