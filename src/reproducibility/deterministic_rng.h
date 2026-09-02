#pragma once

#include "deterministic_seed.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace truetest::reproducibility {

inline constexpr std::uint32_t kDeterministicRngVersion = 1;

// xoshiro256** with SplitMix64 state expansion. The state transition and
// sampling functions below are repository-owned so deterministic paths do
// not depend on implementation-defined std::*_distribution behaviour.
class DeterministicRng final
{
public:
    explicit DeterministicRng(std::uint64_t seed) noexcept
    {
        reset(seed);
    }

    void reset(std::uint64_t seed) noexcept
    {
        std::uint64_t state = seed;
        for (auto& word : state_)
        {
            word = splitmix64(state);
            state = word;
        }
        if ((state_[0] | state_[1] | state_[2] | state_[3]) == 0)
            state_[0] = 1;
        has_spare_normal_ = false;
        spare_normal_ = 0.0;
    }

    [[nodiscard]] std::uint64_t next_u64() noexcept
    {
        const std::uint64_t result = std::rotl(state_[1] * 5ULL, 7) * 9ULL;
        const std::uint64_t shift = state_[1] << 17U;

        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];

        state_[2] ^= shift;
        state_[3] = std::rotl(state_[3], 45);
        return result;
    }

    // Exact mapping of the high 53 PRNG bits to [0, 1). Multiplication by a
    // binary power is exact in IEEE-754 binary64.
    [[nodiscard]] double uniform_unit() noexcept
    {
        constexpr double inverse_2_pow_53 = 0x1.0p-53;
        return static_cast<double>(next_u64() >> 11U) * inverse_2_pow_53;
    }

    [[nodiscard]] double uniform(double lower, double upper) noexcept
    {
        return lower + (upper - lower) * uniform_unit();
    }

    // Lemire-style rejection sampling without modulo bias.
    [[nodiscard]] std::uint64_t uniform_bounded(std::uint64_t bound) noexcept
    {
        if (bound == 0)
            return 0;
        const std::uint64_t threshold = static_cast<std::uint64_t>(-bound) % bound;
        for (;;)
        {
            const std::uint64_t value = next_u64();
            if (value >= threshold)
                return value % bound;
        }
    }

    // Box-Muller with a cached second sample. libm is consequently part of
    // the declared determinism envelope; cross-libm bit identity is not
    // claimed. reset() explicitly clears the cache for object reuse.
    [[nodiscard]] double standard_normal() noexcept
    {
        if (has_spare_normal_)
        {
            has_spare_normal_ = false;
            return spare_normal_;
        }

        constexpr double two_pi = 0x1.921fb54442d18p+2;
        double u1 = 0.0;
        do
        {
            u1 = uniform_unit();
        } while (u1 == 0.0);
        const double u2 = uniform_unit();
        const double radius = std::sqrt(-2.0 * std::log(u1));
        const double angle = two_pi * u2;
        spare_normal_ = radius * std::sin(angle);
        has_spare_normal_ = true;
        return radius * std::cos(angle);
    }

    [[nodiscard]] double normal(double mean, double standard_deviation) noexcept
    {
        return mean + standard_deviation * standard_normal();
    }

private:
    std::array<std::uint64_t, 4> state_{};
    double spare_normal_{0.0};
    bool has_spare_normal_{false};
};

} // namespace truetest::reproducibility
