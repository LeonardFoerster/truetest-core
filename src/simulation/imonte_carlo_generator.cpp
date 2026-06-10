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
        const uint64_t path_seed = base_seed ^ (static_cast<uint64_t>(i) * 0x9e3779b97f4a7c15ULL);
        paths.push_back(generate(path_seed, cfg));
    }
    return paths;
}

} // namespace truetest::simulation
