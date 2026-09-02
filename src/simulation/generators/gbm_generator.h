#pragma once

#include "../imonte_carlo_generator.h"

#include <cstdint>
#include <string>

namespace truetest::simulation {

/**
 * Geometric Brownian Motion generator.
 *
 * Price evolution (Euler-Maruyama on log-price):
 *     log(S_t) = log(S_{t-1}) + (mu - 0.5 * sigma^2) * dt + sigma * sqrt(dt) * Z
 * where Z ~ N(0,1)
 *
 * This produces realistic multiplicative returns and is the standard
 * model used for equity / crypto price paths in Monte Carlo studies.
 */
class GBMGenerator final : public IMonteCarloGenerator {
public:
    GBMGenerator();

    std::string name() const override { return "gbm"; }

    McGeneratorConfig default_config() const override;

    SyntheticPath generate(uint64_t seed,
                           const McGeneratorConfig& cfg) override;

    std::vector<SyntheticPath> generate_batch(
        size_t n_paths,
        uint64_t base_seed,
        const McGeneratorConfig& cfg) override;

private:
    // Internal RNG state is per-call (seeded fresh each generate()).
    // This keeps generators stateless between calls and fully reproducible.
};

} // namespace truetest::simulation
