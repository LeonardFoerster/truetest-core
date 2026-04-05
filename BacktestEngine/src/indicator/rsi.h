#pragma once
#include <optional>

class relative_strength_index
{
public:
    explicit relative_strength_index(std::size_t period = 14)
        : period_(period) {}

    std::optional<double> update(double price)
    {
        if (!has_prev_)
        {
            prev_price_ = price;
            has_prev_ = true;
            return std::nullopt;
        }

        double change = price - prev_price_;
        prev_price_ = price;
        double gain = change > 0.0 ? change : 0.0;
        double loss = change < 0.0 ? -change : 0.0;

        if (!initialized_)
        {
            gain_sum_ += gain;
            loss_sum_ += loss;
            ++count_;
            if (count_ == period_)
            {
                avg_gain_ = gain_sum_ / static_cast<double>(period_);
                avg_loss_ = loss_sum_ / static_cast<double>(period_);
                initialized_ = true;
                return compute_rsi();
            }
            return std::nullopt;
        }

        // Smoothed (Wilder's) moving average
        double dp = static_cast<double>(period_);
        avg_gain_ = (avg_gain_ * (dp - 1.0) + gain) / dp;
        avg_loss_ = (avg_loss_ * (dp - 1.0) + loss) / dp;
        return compute_rsi();
    }

    bool ready() const { return last_value_.has_value(); }
    double value() const { return last_value_.value(); }

private:
    std::size_t period_;
    double prev_price_ = 0.0;
    bool has_prev_ = false;
    double gain_sum_ = 0.0;
    double loss_sum_ = 0.0;
    double avg_gain_ = 0.0;
    double avg_loss_ = 0.0;
    std::size_t count_ = 0;
    bool initialized_ = false;
    std::optional<double> last_value_;

    std::optional<double> compute_rsi()
    {
        if (avg_loss_ == 0.0)
            last_value_ = 100.0;
        else
        {
            double rs = avg_gain_ / avg_loss_;
            last_value_ = 100.0 - (100.0 / (1.0 + rs));
        }
        return last_value_;
    }
};
