#pragma once

#include "providers/provider_event.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace truetest::simulation {

// ---------------------------------------------------------------------------
// Canonical per-trial seed derivation (single source of truth).
// Used by path generators AND MonteCarloController so report seed_used
// always matches the seed that produced the market path (drill-down safe).
// ---------------------------------------------------------------------------
inline constexpr uint64_t kMcDefaultBaseSeed = 0xA5A5C0DE42ULL;
inline constexpr uint64_t kMcTrialSeedMix    = 0x9e3779b97f4a7c15ULL;

/** Map CLI/base seed 0 to a fixed default so campaigns stay reproducible. */
inline uint64_t effective_mc_base_seed(uint64_t base_seed) {
    return base_seed == 0 ? kMcDefaultBaseSeed : base_seed;
}

/**
 * Deterministic per-trial seed from campaign base_seed + trial index.
 * Formula: effective_base ^ (trial_index * golden_ratio_mix)
 */
inline uint64_t derive_mc_trial_seed(uint64_t base_seed, std::size_t trial_index) {
    const uint64_t base = effective_mc_base_seed(base_seed);
    return base ^ (static_cast<uint64_t>(trial_index) * kMcTrialSeedMix);
}

/**
 * Configuration for a Monte Carlo market path generator.
 * Fields are intentionally minimal for Phase 1 (GBM-focused).
 * Future models (OU, jumps, stochastic vol, multi-asset) can extend via
 * additional optional fields or a params map without breaking existing code.
 */
struct McGeneratorConfig {
    std::string symbol = "BTCUSDT";
    double initial_price = 65000.0;

    // Number of discrete steps (bars or ticks) to generate.
    int64_t n_steps = 5000;

    // Time step size (fraction of a year). Example: 1.0 / (252*24*60) for ~1-minute bars.
    double dt = 1.0 / (252.0 * 24.0 * 60.0);

    // Annualized drift (mu) and volatility (sigma) for GBM.
    // For other models these may be reinterpreted (e.g. theta, long_term_mean for OU).
    double mu = 0.0;
    double sigma = 0.02;   // ~2% per sqrt(year) is very low; typical crypto is higher (0.4-1.2)

    // Optional seed override (0 means "derive from caller").
    uint64_t seed = 0;

    // Whether to also populate a simple synthetic L2 ladder around the mid.
    // Phase 1: L2 is basic and stylized (constant spread + noise).
    // Useful for exercising --queue-model and QueueAwareBookAdapter.
    bool emit_synthetic_l2 = false;
    double base_spread_bps = 4.0;
    double depth_noise = 0.15;   // relative stddev on level sizes
};

/**
 * A single generated market path.
 * Designed for direct consumption by existing TrueTest ingestion paths:
 *   - bars  → bar_record_sink / CsvBarParser style
 *   - ticks → tick path
 *   - provider::event variant path (future)
 *
 * Raw mids + volumes are provided as contiguous arrays for cache-friendly
 * consumption by custom consumers or fast path generation.
 */
struct SyntheticPath {
    std::string symbol;

    // Primary outputs for current engine backtest paths
    std::vector<provider::bar> bars;
    std::vector<provider::tick> ticks;

    // Optional L2 for queue-position / realism fidelity testing
    std::vector<provider::l2_snapshot> l2_snapshots;
    std::vector<provider::l2_update> l2_updates;

    // Raw series (contiguous, SoA layout) – convenient for vectorized consumers
    std::vector<double> mids;
    std::vector<double> volumes;

    uint64_t seed_used = 0;   // The actual RNG seed that produced this path
};

// =============================================================================
// Phase 2+: Multi-trial Monte Carlo structures
// =============================================================================

struct TrialResult {
    uint64_t trial_id = 0;
    uint64_t seed_used = 0;

    double initial_equity = 0.0;
    double final_equity = 0.0;
    double total_pnl = 0.0;
    double max_drawdown = 0.0;
    double sharpe_ratio = 0.0;
    std::size_t total_trades = 0;
    std::size_t winning_trades = 0;

    double win_rate = 0.0;       // 0-100 (matches AnalyticsReport convention)
    double profit_factor = 0.0;

    // Full analytics available for detailed reporting
    // (populated when controller is configured to keep full reports)
    // AnalyticsReport full_report;  // forward-declared or included by .cpp users
};

struct McAggregate {
    std::size_t n_trials = 0;

    // P&L statistics
    double mean_pnl = 0.0;
    double median_pnl = 0.0;
    double p5_pnl = 0.0;     // 5th percentile
    double p95_pnl = 0.0;

    // Risk / performance
    double mean_sharpe = 0.0;
    double median_sharpe = 0.0;
    double mean_max_dd = 0.0;
    double worst_max_dd = 0.0;

    double profit_factor_mean = 0.0;
    double median_profit_factor = 0.0;
    double win_rate_mean = 0.0;
    double median_win_rate = 0.0;

    std::size_t trials_with_positive_pnl = 0;
    std::size_t trials_with_profit_factor_gt_1 = 0;

    // Performance (Phase 5)
    double wall_time_ms = 0.0;

    // Raw per-trial summaries (lightweight)
    std::vector<TrialResult> trials;
};

struct McRunConfig {
    std::string generator_name = "gbm";
    McGeneratorConfig generator_config;

    std::size_t n_trials = 100;
    // 0 = use kMcDefaultBaseSeed (fixed, reproducible — never wall-clock).
    uint64_t base_seed = 0;

    std::string strategy_name = "mean-reversion";

    // Strategy set_param() pairs applied once when a strategy is constructed
    // (and preserved across reuse resets). Mirrors CLI --param key=value.
    std::vector<std::pair<std::string, double>> strategy_params;

    // Initial equity for each trial (passed through to engine_config and reports)
    double initial_balance = 10000.0;

    // Realism / engine settings to apply to every trial (must match single-run
    // semantics when CLI sets fee/latency/impact — FR-02; maker-queue — FR-mc-queue).
    double order_latency_us = 0.0;
    double impact_k_bps = 0.0;
    double impact_adv = 0.0;          // ADV for SquareRootImpactModel; 0 = no impact
    std::string fee_model;            // "", "fixed", "tiered"
    double fee_value = 0.0;           // fixed fee per trade
    double maker_rate = 0.0;
    double taker_rate = 0.0;
    double bar_spread_bps = 0.0;      // half-spread adverse slip (mirrors main.inc --bar-spread-bps)
    // Maker-queue / hybrid paper path ("" or "none" = off; uniform|front|back).
    std::string maker_queue_model;
    bool walked_book_impact = false;

    bool keep_full_reports = false;   // if true, TrialResult can carry more data

    // Phase A (MC object reuse): if true, reuse data_handler / strategy between
    // trials. Mutually exclusive with parallel_trials (shared mutable state).
    bool reuse_objects_between_trials = false;

    // Phase 5: Experimental parallel execution.
    // WARNING: Conflicts with engine core pinning and threading presets.
    // Only safe with --thread-preset inline and no --no-pin overrides in some cases.
    // Results collection is thread-safe but final order may not be deterministic.
    // Mutually exclusive with reuse_objects_between_trials.
    bool parallel_trials = false;
    unsigned max_parallel_threads = 0; // 0 = hardware_concurrency()
};

} // namespace truetest::simulation
