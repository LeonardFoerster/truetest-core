#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

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
        : period_(validate_configuration(period, num_std))
        , num_std_(num_std)
        , window_(period_)
    {
        inv_period_ = 1.0 / static_cast<double>(period_);
        inv_period_minus_one_ = period_ > 1 ? 1.0 / static_cast<double>(period_ - 1) : 0.0;
    }

    std::optional<bollinger_result> update(double price)
    {
        // Market-data validation should already guarantee this invariant. Keep
        // the indicator fail-closed as well: an invalid observation must not
        // poison the rolling state and make later finite outputs look valid.
        if (!std::isfinite(price)) throw std::invalid_argument("bollinger price must be finite");

        std::size_t candidate_count = count_;
        double candidate_center = center_;
        double candidate_mean = mean_;
        double candidate_m2 = m2_;
        const bool replacing = candidate_count == period_;
        if (replacing) {
            remove_sample_fixed(window_[next_] - candidate_center, inv_period_minus_one_,
                                candidate_count, candidate_mean, candidate_m2);
        }
        if (candidate_count == 0) candidate_center = price;
        if (replacing) {
            add_sample_fixed(price - candidate_center, inv_period_, candidate_count, candidate_mean,
                             candidate_m2);
        } else {
            add_sample(price - candidate_center, candidate_count, candidate_mean, candidate_m2);
        }

        // A pure rolling downdate accumulates rounding error over an unbounded
        // stream. Build a fresh add-only Welford state from the next complete
        // window after a deterministic cadence. This deamortizes the rebase:
        // every accepted observation still performs O(1) work and there is no
        // O(period) latency spike.
        bool candidate_shadow_active = shadow_active_;
        std::size_t candidate_shadow_count = shadow_count_;
        double candidate_shadow_center = shadow_center_;
        double candidate_shadow_mean = shadow_mean_;
        double candidate_shadow_m2 = shadow_m2_;
        std::size_t candidate_replacements = replacements_since_rebase_;
        if (count_ == period_ && period_ > 1) {
            if (candidate_replacements < rebase_cadence_) ++candidate_replacements;
            if (!candidate_shadow_active && candidate_replacements >= rebase_cadence_) {
                candidate_shadow_active = true;
                candidate_shadow_count = 0;
                candidate_shadow_center = 0.0;
                candidate_shadow_mean = 0.0;
                candidate_shadow_m2 = 0.0;
            }
            if (candidate_shadow_active) {
                if (candidate_shadow_count == 0) candidate_shadow_center = price;
                add_sample(price - candidate_shadow_center, candidate_shadow_count,
                           candidate_shadow_mean, candidate_shadow_m2);
                if (candidate_shadow_count == period_) {
                    candidate_count = candidate_shadow_count;
                    candidate_center = candidate_shadow_center;
                    candidate_mean = candidate_shadow_mean;
                    candidate_m2 = candidate_shadow_m2;
                    candidate_shadow_active = false;
                    candidate_shadow_count = 0;
                    candidate_shadow_center = 0.0;
                    candidate_shadow_mean = 0.0;
                    candidate_shadow_m2 = 0.0;
                    candidate_replacements = 0;
                }
            }
        }

        if (!std::isfinite(candidate_center) || !std::isfinite(candidate_mean) ||
            !std::isfinite(candidate_m2))
            throw std::overflow_error("bollinger rolling moments overflowed");
        if (candidate_m2 < 0.0 && candidate_count != period_)
            throw std::runtime_error("bollinger warmup variance became negative");
        if (candidate_shadow_active &&
            (!std::isfinite(candidate_shadow_center) || !std::isfinite(candidate_shadow_mean) ||
             !std::isfinite(candidate_shadow_m2) || candidate_shadow_m2 < 0.0)) {
            throw std::overflow_error("bollinger shadow rebase moments overflowed");
        }

        std::optional<bollinger_result> candidate_value;
        if (candidate_count == period_) {
            // The remove/add Welford recurrence avoids E[x^2]-E[x]^2
            // cancellation. A negative M2 is neither silently converted into
            // plausible zero volatility nor repaired with an O(period) scan on
            // the hot path: reject it before committing any state.
            if (candidate_m2 < 0.0)
                throw std::runtime_error("bollinger rolling variance became negative");

            const double variance = candidate_m2 / static_cast<double>(candidate_count);
            const double stddev = std::sqrt(variance);
            const double width = num_std_ * stddev;
            const double absolute_mean = candidate_center + candidate_mean;
            const double upper = absolute_mean + width;
            const double lower = absolute_mean - width;
            if (!std::isfinite(absolute_mean) || !std::isfinite(upper) || !std::isfinite(lower)) {
                throw std::overflow_error("bollinger result is outside the finite double domain");
            }

            candidate_value = bollinger_result{absolute_mean, upper, lower};
        }

        // Commit only after the entire candidate window and result have been
        // validated. Any exception above leaves all observable rolling state
        // and shadow-rebase progress unchanged.
        window_[next_] = price;
        ++next_;
        if (next_ == period_) next_ = 0;
        count_ = candidate_count;
        center_ = candidate_center;
        mean_ = candidate_mean;
        m2_ = candidate_m2;
        shadow_active_ = candidate_shadow_active;
        shadow_count_ = candidate_shadow_count;
        shadow_center_ = candidate_shadow_center;
        shadow_mean_ = candidate_shadow_mean;
        shadow_m2_ = candidate_shadow_m2;
        replacements_since_rebase_ = candidate_replacements;
        if (candidate_value) last_value_ = candidate_value;
        return candidate_value;
    }

    bool ready() const { return last_value_.has_value(); }
    bollinger_result value() const { return last_value_.value(); }

private:
    static std::size_t validate_configuration(std::size_t period, double num_std)
    {
        if (period == 0) throw std::invalid_argument("bollinger period must be positive");
        if (!std::isfinite(num_std) || num_std < 0.0)
            throw std::invalid_argument(
                "bollinger standard-deviation multiplier must be finite and non-negative");
        return period;
    }

    static void add_sample(double value, std::size_t& count, double& mean, double& m2) noexcept
    {
        ++count;
        const double delta = value - mean;
        mean += delta / static_cast<double>(count);
        const double delta_after = value - mean;
        m2 += delta * delta_after;
    }

    static void add_sample_fixed(double value, double reciprocal_new_count, std::size_t& count,
                                 double& mean, double& m2) noexcept
    {
        ++count;
        const double delta = value - mean;
        mean += delta * reciprocal_new_count;
        m2 += delta * (value - mean);
    }

    static void remove_sample_fixed(double value, double reciprocal_new_count, std::size_t& count,
                                    double& mean, double& m2) noexcept
    {
        if (count <= 1) {
            count = 0;
            mean = 0.0;
            m2 = 0.0;
            return;
        }

        const auto new_count = count - 1;
        const double old_mean = mean;
        const double new_mean = old_mean - (value - old_mean) * reciprocal_new_count;
        m2 -= (value - old_mean) * (value - new_mean);
        mean = new_mean;
        count = new_count;
    }

    static constexpr std::size_t rebase_cadence_for(std::size_t period) noexcept
    {
        constexpr std::size_t minimum_cadence = 4096;
        constexpr std::size_t max_size = std::numeric_limits<std::size_t>::max();
        const std::size_t scaled = period > max_size / 32 ? max_size : period * 32;
        return scaled > minimum_cadence ? scaled : minimum_cadence;
    }

    std::size_t period_;
    double num_std_;
    // Allocated once at construction; update() performs no heap allocation.
    std::vector<double> window_;
    std::size_t next_ = 0;
    std::size_t count_ = 0;
    double center_ = 0.0;
    // Welford mean is held relative to center_, keeping all steady-state
    // arithmetic near the window's range instead of the absolute price.
    double mean_ = 0.0;
    double m2_ = 0.0;
    double inv_period_ = 0.0;
    double inv_period_minus_one_ = 0.0;
    std::size_t rebase_cadence_ = rebase_cadence_for(period_);
    std::size_t replacements_since_rebase_ = 0;
    bool shadow_active_ = false;
    std::size_t shadow_count_ = 0;
    double shadow_center_ = 0.0;
    double shadow_mean_ = 0.0;
    double shadow_m2_ = 0.0;
    std::optional<bollinger_result> last_value_;
};
