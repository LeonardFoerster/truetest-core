#pragma once

#include <cmath>
#include <optional>
#include <queue>
#include <algorithm>
#include <limits>

class average_true_range
{
public:
    explicit average_true_range(std::size_t period = 14) : period_(period) {}

    // Stateful update: automatically tracks previous close between calls.
    // Returns ATR value once ready (after period bars).
    std::optional<double> update(double high, double low, double close)
    {
        std::optional<double> prev_c = last_close_.has_value()
            ? std::optional<double>(last_close_.value())
            : std::nullopt;

        double tr = compute_tr(high, low, close, prev_c);

        if (!initialized_)
        {
            tr_window_.push(tr);
            tr_sum_ += tr;
            if (tr_window_.size() > period_)
            {
                tr_sum_ -= tr_window_.front();
                tr_window_.pop();
            }
            if (tr_window_.size() == period_)
            {
                last_atr_ = tr_sum_ / static_cast<double>(period_);
                initialized_ = true;
            }
        }
        else
        {
            // Wilder's smoothing (standard for ATR)
            last_atr_ = ((*last_atr_) * (static_cast<double>(period_) - 1.0) + tr) / static_cast<double>(period_);
        }

        last_close_ = close;
        last_tr_ = tr;
        return last_atr_;
    }

    // Explicit prev_close version if needed for first bar control
    std::optional<double> update(double high, double low, double close, std::optional<double> prev_close)
    {
        double tr = compute_tr(high, low, close, prev_close);
        // For explicit, still feed into state (rarely used)
        if (!initialized_)
        {
            tr_window_.push(tr);
            tr_sum_ += tr;
            if (tr_window_.size() > period_)
            {
                tr_sum_ -= tr_window_.front();
                tr_window_.pop();
            }
            if (tr_window_.size() == period_)
            {
                last_atr_ = tr_sum_ / static_cast<double>(period_);
                initialized_ = true;
            }
        }
        else
        {
            last_atr_ = ((*last_atr_) * (static_cast<double>(period_) - 1.0) + tr) / static_cast<double>(period_);
        }
        last_close_ = close;
        last_tr_ = tr;
        return last_atr_;
    }

    bool ready() const { return initialized_ && last_atr_.has_value(); }
    double value() const { return last_atr_.value_or(0.0); }
    double last_true_range() const { return last_tr_; }

    void reset()
    {
        tr_window_ = {};
        tr_sum_ = 0.0;
        last_atr_.reset();
        last_close_.reset();
        last_tr_ = 0.0;
        initialized_ = false;
    }

private:
    static double compute_tr(double high, double low, double /*close*/, std::optional<double> prev_close)
    {
        double hl = high - low;
        if (!prev_close.has_value())
            return hl;
        double hc = std::abs(high - *prev_close);
        double lc = std::abs(low - *prev_close);
        return std::max({hl, hc, lc});
    }

    std::size_t period_;
    std::queue<double> tr_window_;
    double tr_sum_ = 0.0;
    std::optional<double> last_atr_;
    std::optional<double> last_close_;
    double last_tr_ = 0.0;
    bool initialized_ = false;
};
