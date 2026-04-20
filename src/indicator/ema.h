#pragma once
#include <optional>

class exponential_moving_average
{
public:
    explicit exponential_moving_average(std::size_t period)
        : period_(period), k_(2.0 / (static_cast<double>(period) + 1.0)) {}

    std::optional<double> update(double price)
    {
        if (!initialized_)
        {
            sum_ += price;
            ++count_;
            if (count_ == period_)
            {
                last_value_ = sum_ / static_cast<double>(period_);
                initialized_ = true;
                return last_value_;
            }
            return std::nullopt;
        }
        last_value_ = price * k_ + last_value_.value() * (1.0 - k_);
        return last_value_;
    }

    bool ready() const { return last_value_.has_value(); }
    double value() const { return last_value_.value(); }

private:
    std::size_t period_;
    double k_;
    std::optional<double> last_value_;
    double sum_ = 0.0;
    std::size_t count_ = 0;
    bool initialized_ = false;
};
