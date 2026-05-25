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

} // namespace truetest::simulation
