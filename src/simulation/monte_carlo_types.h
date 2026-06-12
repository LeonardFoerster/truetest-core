#pragma once

#include "providers/provider_event.h"

#include <cstdint>
#include <string>
#include <vector>

namespace truetest::simulation {

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
    uint64_t base_seed = 0;           // 0 = derive from time or fixed default

    std::string strategy_name = "mean-reversion";

    // Initial equity for each trial (passed through to engine_config and reports)
    double initial_balance = 10000.0;

    // Realism / engine settings to apply to every trial
    double order_latency_us = 0.0;
    double impact_k_bps = 0.0;

    bool keep_full_reports = false;   // if true, TrialResult can carry more data

    // Phase A (MC object reuse): if true, reuse data_handler / portfolio / Analytics / ExitManager
    // between trials instead of allocating fresh ones each time.
    bool reuse_objects_between_trials = false;

    // Phase 5: Experimental parallel execution.
    // WARNING: Conflicts with engine core pinning and threading presets.
    // Only safe with --thread-preset inline and no --no-pin overrides in some cases.
    // Results collection is thread-safe but final order may not be deterministic.
    bool parallel_trials = false;
    unsigned max_parallel_threads = 0; // 0 = hardware_concurrency()
};

} // namespace truetest::simulation
