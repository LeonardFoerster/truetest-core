#pragma once
#include <queue>
#include <optional>

class simple_moving_average 
{
public:
    explicit simple_moving_average(std::size_t period) : period_(period) {}

    std::optional<double> update(double price)
    {
        sum_ += price;
        window_.push(price);
        if (window_.size() > period_)
        {
            sum_ -= window_.front();
            window_.pop();
        }
        if (window_.size() == period_)
        {
            last_value_ = sum_ / static_cast<double>(period_);
            return last_value_;
        }
        return std::nullopt;
    }

    // Whether enough data has been accumulated to produce a value.
    bool ready() const { return last_value_.has_value(); }

    // Last computed SMA value. Only valid when ready() is true.
    double value() const { return last_value_.value(); }

private:
    std::optional<double> last_value_;
    std::size_t period_;
    std::queue<double> window_;
    double sum_ = 0.0;
};
