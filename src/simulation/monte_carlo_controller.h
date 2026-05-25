#pragma once

#include "monte_carlo_types.h"
#include "imonte_carlo_generator.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace truetest::simulation {

/**
 * High-level orchestrator for running Monte Carlo campaigns.
 *
 * Responsibilities:
 *   - Owns or creates the IMonteCarloGenerator for the chosen model
 *   - Runs N independent trials with deterministic per-trial seeding
 *   - Ensures clean isolation between trials (fresh data_handler, engine, strategy)
 *   - Collects lightweight TrialResult + optional full AnalyticsReport
 *   - Produces McAggregate summary
 *
 * This class is the core of Phase 2. It is designed to be usable both
 * programmatically (from tests or future C API) and later from the CLI.
 */
class MonteCarloController {
public:
    explicit MonteCarloController(const McRunConfig& run_config);

    /**
     * Execute all trials and return the aggregate result.
     * This is the main entry point for Phase 2.
     *
     * If McRunConfig::parallel_trials is true, uses std::jthread for
     * concurrent execution (Phase 5 experimental feature).
     */
    McAggregate run();

    const McRunConfig& config() const { return config_; }

private:
    McRunConfig config_;
    std::unique_ptr<IMonteCarloGenerator> generator_;

    void ensure_generator();
    uint64_t derive_trial_seed(std::size_t trial_index) const;

    // Internal helper that runs exactly one trial and returns its result.
    TrialResult run_single_trial(std::size_t trial_index);

    // Phase 5 optimization: version that accepts a pre-generated path from batch generation.
    TrialResult run_single_trial_with_path(std::size_t trial_index, SyntheticPath path);
};

} // namespace truetest::simulation
