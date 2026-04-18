#pragma once
#include <cmath>
#include <optional>
#include <queue>

struct bollinger_result
{
    double middle;
    double upper;
    double lower;
};

class bollinger_bands
{
public:
    explicit bollinger_bands(std::size_t period = 20, double num_std = 2.0)
        : period_(period), num_std_(num_std) {}

    std::optional<bollinger_result> update(double price)
    {
        sum_ += price;
        sum_sq_ += price * price;
        window_.push(price);

        if (window_.size() > period_)
        {
            double old = window_.front();
            window_.pop();
            sum_ -= old;
            sum_sq_ -= old * old;
        }

        if (window_.size() == period_)
        {
            double n = static_cast<double>(period_);
            double mean = sum_ / n;
            double variance = (sum_sq_ / n) - (mean * mean);
            if (variance < 0.0) variance = 0.0;
            double stddev = std::sqrt(variance);

            last_value_ = {mean, mean + num_std_ * stddev, mean - num_std_ * stddev};
            return last_value_;
        }
        return std::nullopt;
    }

    bool ready() const { return last_value_.has_value(); }
    bollinger_result value() const { return last_value_.value(); }

private:
    std::size_t period_;
    double num_std_;
    std::queue<double> window_;
    double sum_ = 0.0;
    double sum_sq_ = 0.0;
    std::optional<bollinger_result> last_value_;
};
