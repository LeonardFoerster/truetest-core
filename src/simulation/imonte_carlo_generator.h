#pragma once

#include "monte_carlo_types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace truetest::simulation {

/**
 * Abstract interface for stochastic market path generators used by the
 * Monte Carlo simulation engine.
 *
 * Implementations must be:
 *   - Fully deterministic given the same seed + config
 *   - Thread-safe for read-only use after construction (generation may be called from multiple threads)
 *   - Cache-friendly in their output layout (contiguous vectors)
 *
 * Phase 1 focus: single-path generation sufficient for SyntheticProvider.
 * generate_batch() has a default implementation that calls generate() in a loop.
 */
class IMonteCarloGenerator {
public:
    virtual ~IMonteCarloGenerator();

    virtual std::string name() const = 0;

    virtual McGeneratorConfig default_config() const = 0;

    /**
     * Generate one reproducible path.
     * The returned SyntheticPath must be usable directly with:
     *   - DataBridge + existing bar/tick sinks
     *   - engine::run() / run_tick_data()
     *   - All realism models (latency, queue, impact, etc.)
     */
    virtual SyntheticPath generate(uint64_t seed,
                                   const McGeneratorConfig& cfg) = 0;

    /**
     * Generate multiple independent paths from a base seed.
     * Default implementation loops over generate(derive_mc_trial_seed(base, i), cfg).
     * Concrete generators must use the same seed formula so seed_used is
     * drill-down compatible with MonteCarloController.
     */
    virtual std::vector<SyntheticPath> generate_batch(
        size_t n_paths,
        uint64_t base_seed,
        const McGeneratorConfig& cfg);

    // Future extension points (not required in Phase 1):
    // virtual void warm_up(uint64_t seed, const McGeneratorConfig& cfg) {}
    // virtual bool supports_simd() const { return false; }
};

} // namespace truetest::simulation
