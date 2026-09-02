#pragma once

#include <cstdint>
#include <type_traits>

namespace truetest::reproducibility {

inline constexpr std::uint32_t kSeedDerivationVersion = 1;

// Stable wire identifiers. Values are part of the deterministic-run schema;
// never reorder or reuse them for a different stochastic component.
enum class SeedDomain : std::uint64_t {
    run = 0x52554e5f56315f01ULL,
    monte_carlo_trial = 0x4d435f545249414cULL,
    strategy = 0x5354524154454759ULL,
    market_maker = 0x4d41524b45544d4dULL,
    synthetic_price = 0x53594e5f50524943ULL,
    synthetic_volume = 0x53594e5f564f4c55ULL,
    synthetic_l2 = 0x53594e5f4c325f31ULL,
    queue_model = 0x51554555455f4d31ULL,
    fill_model = 0x46494c4c5f4d4f44ULL,
    latency_model = 0x4c4154454e435931ULL,
    impact_model = 0x494d504143545f31ULL,
};

// One SplitMix64 round. This exact algorithm and its unsigned wraparound are
// part of seed-derivation schema v1.
[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t input) noexcept
{
    std::uint64_t value = input + 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

class DeterministicSeedDeriver final
{
public:
    explicit constexpr DeterministicSeedDeriver(std::uint64_t master_seed) noexcept
        : master_seed_(master_seed)
    {
    }

    [[nodiscard]] constexpr std::uint64_t master_seed() const noexcept
    {
        return master_seed_;
    }

    // Derivation is a pure function of master seed, stable domain identifier,
    // and stable index. It never consumes shared state, so construction order
    // and worker scheduling cannot perturb the result.
    [[nodiscard]] constexpr std::uint64_t derive(
        SeedDomain domain, std::uint64_t stable_index = 0) const noexcept
    {
        constexpr std::uint64_t namespace_v1 = 0x5454523653454544ULL;
        std::uint64_t value = splitmix64(master_seed_ ^ namespace_v1);
        value = splitmix64(value ^ static_cast<std::uint64_t>(domain));
        return splitmix64(value ^ splitmix64(stable_index));
    }

    [[nodiscard]] constexpr std::uint64_t trial_seed(
        std::uint64_t trial_index) const noexcept
    {
        return derive(SeedDomain::monte_carlo_trial, trial_index);
    }

    [[nodiscard]] constexpr std::uint64_t trial_component_seed(
        std::uint64_t trial_index, SeedDomain component) const noexcept
    {
        return DeterministicSeedDeriver(trial_seed(trial_index)).derive(component);
    }

private:
    std::uint64_t master_seed_;
};

static_assert(std::is_trivially_copyable_v<DeterministicSeedDeriver>);

} // namespace truetest::reproducibility
