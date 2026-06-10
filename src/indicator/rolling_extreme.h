#pragma once
#include <algorithm>
#include <deque>

// Rolling minimum / maximum over the last `period` values, inclusive of the
// most recently pushed value. Mirrors pandas' rolling(period).min()/.max():
// `ready()` becomes true once `period` values have been observed.
//
// Period is small in practice (e.g. 7), so the extremes are computed with a
// linear scan over the window rather than a monotonic deque — clearer and the
// constant factor is irrelevant at this size.
class rolling_extreme
{
public:
    explicit rolling_extreme(std::size_t period) : period_(period) {}

    void update(double value)
    {
        window_.push_back(value);
        if (window_.size() > period_)
            window_.pop_front();
    }

    bool ready() const { return window_.size() == period_; }

    double min() const { return *std::min_element(window_.begin(), window_.end()); }
    double max() const { return *std::max_element(window_.begin(), window_.end()); }

    void reset() { window_.clear(); }

private:
    std::size_t period_;
    std::deque<double> window_;
};
