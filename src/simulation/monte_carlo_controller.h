#pragma once

#include "monte_carlo_types.h"
#include "imonte_carlo_generator.h"

#include "data/data_handler.h"
#include "execution/portfolio.h"
#include "analytics/analytics.h"
#include "exits/exit_manager.h"
#include "strategy/strategy_interface.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace truetest::simulation {

// Pure serial reduction used by MonteCarloController and cheap metric tests.
// Trial order is retained so callers can drill down by trial_id afterwards.
void summarize_monte_carlo_trials(McAggregate& aggregate);

/**
 * High-level orchestrator for running Monte Carlo campaigns.
 *
 * Responsibilities:
 *   - Owns or creates the IMonteCarloGenerator for the chosen model
 *   - Runs N independent trials with deterministic per-trial seeding
 *     (derive_mc_trial_seed — same formula as path generators)
 *   - Fresh engine per trial (engine reuse is incomplete; not used)
 *   - Optional data_handler/strategy reuse only when strategy.supports_mc_trial_reuse()
 *   - Applies strategy_params on construct; fails hard on unknown strategy/generator
 *   - Collects lightweight TrialResult + optional full AnalyticsReport
 *   - Produces McAggregate summary
 *
 * parallel_trials and reuse_objects_between_trials are mutually exclusive.
 * Strategies without supports_mc_trial_reuse() refuse --mc-reuse-objects.
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

private:
    // Phase A (object reuse)
    std::shared_ptr<data_handler> reusable_data_handler_;
    std::shared_ptr<IStrategy>    reusable_strategy_;

    // Note: With Phase 4 deepdive reset hardening, portfolio/Analytics/ExitManager
    // (and more) reuse is now supported via engine::reset_for_next_trial().
    // Rings/workers left untouched per original design (see engine reset comments).
    // Full bit-identical results not guaranteed across all internals.
};

} // namespace truetest::simulation
