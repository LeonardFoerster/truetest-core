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
            return sum_ / static_cast<double>(period_);
        }
        return std::nullopt; 
    }

private:
    std::size_t period_;
    std::queue<double> window_;
    double sum_ = 0.0;
};
