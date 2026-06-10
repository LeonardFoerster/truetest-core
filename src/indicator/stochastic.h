#pragma once

#include "sma.h"

#include <deque>
#include <optional>

struct stochastic_values
{
    double k     = 0.0;   // smoothed %K (main line, after k_smoothing)
    double d     = 0.0;   // %D (signal line)
    double raw_k = 0.0;   // raw (unsmoothed) %K over k_period
};

/**
 * Stochastic Oscillator (classic Lane's Stochastic).
 *
 * Typical usage: Stochastic(5,3,3)
 *   - k_period     : lookback for Highest High / Lowest Low (usually 5 or 14)
 *   - k_smoothing  : SMA period applied to raw %K (usually 3)
 *   - d_period     : SMA period for %D (signal line, usually 3)
 *
 * The class is intentionally lightweight and header-only, consistent with
 * ema.h / rsi.h / sma.h in this project.
 */
class stochastic_oscillator
{
public:
    stochastic_oscillator(std::size_t k_period     = 5,
                          std::size_t k_smoothing  = 3,
                          std::size_t d_period     = 3)
        : k_period_(k_period)
        , k_smoothing_(k_smoothing)
        , d_period_(d_period)
        , raw_k_sma_(k_smoothing)
        , d_sma_(d_period)
    {
    }

    /**
     * Feed one bar. Returns a value only after sufficient warmup.
     * Warmup requirement: at least k_period bars for the range,
     * plus k_smoothing + d_period bars for the two SMAs.
     */
    std::optional<stochastic_values> update(double high, double low, double close)
    {
        highs_.push_back(high);
        lows_.push_back(low);
        closes_.push_back(close);

        // Keep only what we need
        if (highs_.size() > k_period_)
        {
            highs_.pop_front();
            lows_.pop_front();
            closes_.pop_front();
        }

        if (highs_.size() < k_period_)
            return std::nullopt;   // not enough data for raw %K

        // Compute raw %K
        double hh = *std::max_element(highs_.begin(), highs_.end());
        double ll = *std::min_element(lows_.begin(), lows_.end());

        double range = hh - ll;
        double raw_k = (range > 0.0)
                           ? 100.0 * (close - ll) / range
                           : 50.0;   // degenerate case (flat range) -> neutral

        // First smoothing stage: SMA on raw %K
        auto smoothed_k_opt = raw_k_sma_.update(raw_k);
        if (!smoothed_k_opt)
            return std::nullopt;   // still warming up the k_smoothing SMA

        double smoothed_k = *smoothed_k_opt;

        // Second smoothing stage: SMA on smoothed %K → %D
        auto d_opt = d_sma_.update(smoothed_k);
        if (!d_opt)
            return std::nullopt;   // still warming up the d_period SMA

        last_values_.k     = smoothed_k;
        last_values_.d     = *d_opt;
        last_values_.raw_k = raw_k;

        return last_values_;
    }

    bool ready() const { return d_sma_.ready(); }

    double k()     const { return last_values_.k; }
    double d()     const { return last_values_.d; }
    double raw_k() const { return last_values_.raw_k; }

    // Phase A (Monte Carlo object reuse)
    void reset()
    {
        highs_.clear();
        lows_.clear();
        closes_.clear();

        raw_k_sma_ = simple_moving_average(k_smoothing_);
        d_sma_     = simple_moving_average(d_period_);

        last_values_ = stochastic_values{};
    }

    // Accessors for the periods (useful for diagnostics / param exposure)
    std::size_t k_period()     const { return k_period_; }
    std::size_t k_smoothing()  const { return k_smoothing_; }
    std::size_t d_period()     const { return d_period_; }

private:
    std::size_t k_period_;
    std::size_t k_smoothing_;
    std::size_t d_period_;

    std::deque<double> highs_;
    std::deque<double> lows_;
    std::deque<double> closes_;   // currently only needed for the latest close; kept for symmetry / future use

    simple_moving_average raw_k_sma_;
    simple_moving_average d_sma_;

    stochastic_values last_values_{};
};
