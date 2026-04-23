#pragma once

#include <algorithm>

// Cancel-position heuristics: L2 shrinkage minus observed trades is the
// cancel volume, but the venue doesn't tell us where in the queue those
// cancels sat. Front/Back bracket the optimistic/pessimistic extremes;
// Uniform is the realistic baseline. A strategy profitable under Back
// (no queue advance from cancels) is robust; one that only works under
// Front is overfit.
class IQueueModel
{
public:
    virtual ~IQueueModel() = default;
    virtual double update_on_cancels(double size_ahead,
                                     double total_size,
                                     double cancels) = 0;
};

// Optimistic: every cancel advances us one-for-one.
class FrontCancelModel : public IQueueModel
{
public:
    double update_on_cancels(double size_ahead,
                             double /*total_size*/,
                             double cancels) override
    {
        return std::max(0.0, size_ahead - cancels);
    }
};

// Pessimistic: cancels never advance us; trades are the only movement.
class BackCancelModel : public IQueueModel
{
public:
    double update_on_cancels(double size_ahead,
                             double /*total_size*/,
                             double /*cancels*/) override
    {
        return size_ahead;
    }
};

// Realistic: an order at position p in a level of size N absorbs p/N
// of cancels.
class UniformCancelModel : public IQueueModel
{
public:
    double update_on_cancels(double size_ahead,
                             double total_size,
                             double cancels) override
    {
        if (total_size <= 0.0 || cancels <= 0.0) return size_ahead;
        const double frac = size_ahead / total_size;
        return std::max(0.0, size_ahead - cancels * frac);
    }
};
