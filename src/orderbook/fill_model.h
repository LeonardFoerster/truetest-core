#pragma once

#include "../core/event.h"
#include <algorithm>
#include <cmath>

class IFillModel
{
public:
    virtual ~IFillModel() = default;
    virtual double get_fade_rate() const = 0;
    virtual double get_fill_probability(order_side side, double distance_from_mid) const = 0;
};

class PerfectFillModel : public IFillModel
{
public:
    double get_fade_rate() const override { return 0.0; }
    double get_fill_probability(order_side /*side*/, double /*distance_from_mid*/) const override
    {
        return 1.0;
    }
};

class RealisticFillModel : public IFillModel
{
public:
    explicit RealisticFillModel(double fade_rate = 0.1,
                                double base_fill_prob = 0.95,
                                double distance_decay = 10.0)
        : fade_rate_(std::clamp(fade_rate, 0.0, 1.0))
        , base_fill_prob_(std::clamp(base_fill_prob, 0.0, 1.0))
        , distance_decay_(distance_decay) {}

    double get_fade_rate() const override { return fade_rate_; }

    double get_fill_probability(order_side /*side*/, double distance_from_mid) const override
    {
        return base_fill_prob_ * std::exp(-distance_decay_ * std::abs(distance_from_mid));
    }

private:
    double fade_rate_;
    double base_fill_prob_;
    double distance_decay_;
};
