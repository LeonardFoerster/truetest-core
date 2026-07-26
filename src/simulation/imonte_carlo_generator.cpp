#include "imonte_carlo_generator.h"

#include "monte_carlo_types.h"

namespace truetest::simulation {

IMonteCarloGenerator::~IMonteCarloGenerator() = default;

std::vector<SyntheticPath> IMonteCarloGenerator::generate_batch(
    size_t n_paths,
    uint64_t base_seed,
    const McGeneratorConfig& cfg) {

    std::vector<SyntheticPath> paths;
    paths.reserve(n_paths);
    for (size_t i = 0; i < n_paths; ++i) {
        // Must match MonteCarloController::derive_trial_seed / derive_mc_trial_seed
        paths.push_back(generate(derive_mc_trial_seed(base_seed, i), cfg));
    }
    return paths;
}

} // namespace truetest::simulation
