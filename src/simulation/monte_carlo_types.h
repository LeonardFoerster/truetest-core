#pragma once

#include "execution/instrument.h"
#include "engine/risk_limits_contract.h"
#include "exits/default_exit_policy.h"
#include "providers/provider_event.h"
#include "reproducibility/deterministic_seed.h"

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace truetest::simulation {

/**
 * Deterministic per-trial seed from an explicitly supplied campaign master
 * seed and stable trial index. Zero is a valid explicit master seed; absence
 * is represented separately in McRunConfig::master_seed_explicitly_set.
 */
inline uint64_t derive_mc_trial_seed(uint64_t base_seed, std::size_t trial_index) {
    return reproducibility::DeterministicSeedDeriver(base_seed).trial_seed(
        static_cast<std::uint64_t>(trial_index));
}

// Provider-facing deterministic seed boundary. Providers may depend on the
// simulation contract, but must not acquire an upward dependency on the
// reproducibility module merely to domain-separate the synthetic price stream.
inline uint64_t derive_synthetic_price_seed(uint64_t master_seed) {
    return reproducibility::DeterministicSeedDeriver(master_seed).derive(
        reproducibility::SeedDomain::synthetic_price);
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
    double dt = 1.0 / (365.0 * 24.0 * 60.0);

    // Annualized drift (mu) and volatility (sigma) for GBM.
    // For other models these may be reinterpreted (e.g. theta, long_term_mean for OU).
    double mu = 0.0;
    double sigma = 0.65;

    // Optional standalone-generator seed value. Campaign callers always pass
    // the derived trial seed directly to generate(); zero is a valid value.
    uint64_t seed = 0;

    // Whether to also populate a simple synthetic L2 ladder around the mid.
    // Phase 1: L2 is basic and stylized (constant spread + noise).
    // Useful for exercising --queue-model and QueueAwareBookAdapter.
    bool emit_synthetic_l2 = false;
    double base_spread_bps = 4.0;
    double depth_noise = 0.15;   // relative stddev on level sizes
};

inline constexpr std::int64_t kMaxMcGeneratorSteps = 10'000'000;
inline constexpr std::size_t kMaxMcGeneratorPaths = 100'000;
inline constexpr std::size_t kMaxMcGeneratedPointsPerBatch = 10'000'000;
inline constexpr std::int64_t kMcCalendarYearMilliseconds =
    365LL * 24LL * 60LL * 60LL * 1'000LL;
inline constexpr std::int64_t kSyntheticFirstCloseTimeMs =
    1'704'067'200'000LL;
// Synthetic campaigns historically admitted fractional quantities at the
// engine's default 1e8 quantity scale.  The deterministic instrument snapshot
// must make that granularity explicit without silently rounding every
// sub-unit strategy order to zero.
inline constexpr double kSyntheticDefaultLotSize = 1e-8;

inline std::int64_t mc_step_interval_ms(const McGeneratorConfig& cfg)
{
    const long double exact = static_cast<long double>(cfg.dt)
        * static_cast<long double>(kMcCalendarYearMilliseconds);
    const long double rounded = std::floor(exact + 0.5L);
    if (!std::isfinite(exact) || exact < 0.5L
        || rounded > static_cast<long double>(
            std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument(
            "MC dt cannot be represented as a positive millisecond interval");
    return static_cast<std::int64_t>(rounded);
}

inline void validate_mc_generator_config(const McGeneratorConfig& cfg)
{
    if (cfg.symbol.empty())
        throw std::invalid_argument("MC symbol must not be empty");
    for (const unsigned char c : cfg.symbol)
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n'
            || c == '\f' || c == '\v')
            throw std::invalid_argument(
                "MC symbol contains unsupported whitespace or CSV delimiters");
    if (!(cfg.initial_price > 0.0) || !std::isfinite(cfg.initial_price))
        throw std::invalid_argument(
            "MC initial_price must be finite and positive");
    if (cfg.n_steps <= 0 || cfg.n_steps > kMaxMcGeneratorSteps)
        throw std::invalid_argument("MC n_steps is outside supported range");
    if (!(cfg.dt > 0.0) || !std::isfinite(cfg.dt))
        throw std::invalid_argument("MC dt must be finite and positive");
    const std::int64_t step_interval_ms = mc_step_interval_ms(cfg);
    const long double final_offset = static_cast<long double>(
        cfg.n_steps - 1) * static_cast<long double>(step_interval_ms);
    if (final_offset > static_cast<long double>(
            std::numeric_limits<std::int64_t>::max()
            - kSyntheticFirstCloseTimeMs))
        throw std::invalid_argument(
            "MC dt and n_steps exceed the supported timestamp range");
    if (!std::isfinite(cfg.mu))
        throw std::invalid_argument("MC mu must be finite");
    if (!(cfg.sigma >= 0.0) || !std::isfinite(cfg.sigma))
        throw std::invalid_argument(
            "MC sigma must be finite and non-negative");
    if (!(cfg.base_spread_bps >= 0.0)
        || !std::isfinite(cfg.base_spread_bps))
        throw std::invalid_argument(
            "MC base_spread_bps must be finite and non-negative");
    if (!(cfg.depth_noise >= 0.0) || !std::isfinite(cfg.depth_noise))
        throw std::invalid_argument(
            "MC depth_noise must be finite and non-negative");
}

inline void validate_mc_batch_capacity(std::size_t n_paths,
                                       const McGeneratorConfig& cfg)
{
    validate_mc_generator_config(cfg);
    if (n_paths == 0 || n_paths > kMaxMcGeneratorPaths)
        throw std::invalid_argument(
            "MC path count is outside supported range");
    const auto steps = static_cast<std::size_t>(cfg.n_steps);
    if (steps > kMaxMcGeneratedPointsPerBatch / n_paths)
        throw std::invalid_argument(
            "MC batch exceeds supported aggregate path capacity");
}

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
    bool sharpe_ratio_valid = false;
    bool accounting_reconciled = false;
    std::size_t total_trades = 0;
    std::size_t winning_trades = 0;

    double win_rate = 0.0;       // 0-100 (matches AnalyticsReport convention)
    double profit_factor = 0.0;
    bool profit_factor_valid = false;
    bool profit_factor_unbounded = false;
    double total_win = 0.0;      // Gross positive closed-trade P&L
    double total_loss = 0.0;     // Absolute gross negative closed-trade P&L

    // Deterministic artifact identities. Empty means this was an explicitly
    // exploratory campaign without per-trial persistence.
    std::string event_log_sha256;
    std::string lifecycle_sha256;
    std::string synthetic_l2_sha256;
    std::string trial_result_sha256;

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
    std::size_t valid_sharpe_trials = 0;
    double mean_max_dd = 0.0;
    double worst_max_dd = 0.0;

    // Campaign PF pools the underlying trade ledger across trials. Numeric
    // values contain finite ratios only; a zero-loss ledger is represented
    // by the explicit unbounded flag/count, never by a numeric sentinel.
    double profit_factor_pooled = 0.0;
    double profit_factor_mean = 0.0;
    double median_profit_factor = 0.0;
    double profit_factor_mean_valid = 0.0;
    double median_profit_factor_valid = 0.0;
    bool profit_factor_pooled_unbounded = false;
    double win_rate_mean = 0.0;
    double median_win_rate = 0.0;

    std::size_t trials_with_positive_pnl = 0;
    std::size_t trials_with_profit_factor_gt_1 = 0;
    std::size_t valid_profit_factor_trials = 0;
    std::size_t unbounded_profit_factor_trials = 0;

    // Performance (Phase 5)
    double wall_time_ms = 0.0;

    // Raw per-trial summaries (lightweight)
    std::vector<TrialResult> trials;
};

struct McRunConfig {
    std::string generator_name = "gbm";
    McGeneratorConfig generator_config;

    std::size_t n_trials = 100;
    // Zero is a valid seed. The presence bit prevents a missing seed from
    // being confused with that value at the startup boundary.
    uint64_t base_seed = 0;
    bool master_seed_explicitly_set = false;

    std::string strategy_name = "mean-reversion";

    // Strategy set_param() pairs applied once when a strategy is constructed
    // (and preserved across reuse resets). Mirrors CLI --param key=value.
    std::vector<std::pair<std::string, double>> strategy_params;

    // Initial equity for each trial (passed through to engine_config and reports)
    double initial_balance = 10000.0;
    // Platform sizing default. Applied before strategy_params so an explicit
    // --param risk_fraction=... remains the most specific override.
    double risk_fraction = 0.02;
    // Compatibility default used by the CLI for the primary sma and
    // mean-reversion strategies. It is applied before explicit --param
    // overrides, matching the single-run composition root.
    std::size_t sma_period = 20;

    // Complete effective engine inputs for each trial. These are values, not
    // runtime-owned model pointers, so manifests can round-trip them exactly.
    std::size_t rolling_window = 252;
    double risk_free_rate = 0.0;
    std::size_t periods_per_year = 525600;
    std::size_t max_equity_points = 100000;
    std::size_t execution_bar_delay = 1;
    double market_aggression = 1.1;
    double quantity_scale = 1e8;
    truetest::exits::default_exit_params exit_defaults{};
    risk_limits risk{};
    bool risk_unwind = false;
    bool risk_soft_portfolio_limits = true;
    instrument_spec instrument{
        .symbol = "BTCUSDT",
        .tick_size = 0.0,
        .lot_size = kSyntheticDefaultLotSize,
        .min_qty = 0.0,
        .min_notional = 0.0,
        .maker_rate = 0.0,
        .taker_rate = 0.0,
    };

    // Realism / engine settings to apply to every trial (must match single-run
    // semantics when CLI sets fee/latency/impact — FR-02; maker-queue — FR-mc-queue).
    double order_latency_us = 0.0;
    double order_latency_stddev_us = 0.0;
    double wire_latency_us = 0.0;
    double impact_k_bps = 0.0;
    double impact_adv = 0.0;          // ADV for SquareRootImpactModel; 0 = no impact
    std::string fee_model;            // "", "fixed", "tiered"
    double fee_value = 0.0;           // fixed fee per trade
    double maker_rate = 0.0;
    double taker_rate = 0.0;
    double bar_spread_bps = 0.0;      // deprecated; retained so an explicit operator estimate still wins
    // F-04: synthetic-book half spread (mirrors main.inc --mm-spread-pct).
    // This, not bar_spread_bps, is what a market entry actually pays.
    double mm_spread_pct = 0.002;
    int mm_levels_per_side = 10;
    int mm_base_depth = 100;
    double mm_vol_spread_mult = 0.25;
    double mm_max_half_spread_pct = 0.05;

    double fill_probability = 0.0;
    double fill_fade = 0.0;
    double fill_decay = 10.0;

    // Maker-queue / hybrid paper path ("" or "none" = off; uniform|front|back).
    std::string maker_queue_model;
    bool walked_book_impact = false;

    bool keep_full_reports = false;   // if true, TrialResult can carry more data

    // Phase A (MC object reuse): if true, reuse data_handler / strategy between
    // trials. Mutually exclusive with parallel_trials (shared mutable state).
    bool reuse_objects_between_trials = false;

    // Parallel trial scheduling. Each trial owns its RNG state and artifacts;
    // aggregation is stable-index ordered. Full persistence additionally owns
    // one unpinned logging worker per active trial (captured in the manifest).
    // Mutually exclusive with reuse_objects_between_trials.
    bool parallel_trials = false;
    // Exploratory runs may use 0 = hardware_concurrency(). Manifest-backed
    // deterministic runs reject 0 and record the explicit worker count.
    unsigned max_parallel_threads = 0;

    // Full deterministic/golden persistence. The directory is a campaign
    // root; each trial owns trials/trial_<stable-index>/ below it. This mode
    // is intentionally opt-in for legacy exploratory campaigns, but is
    // mandatory for manifest-backed runs.
    bool persist_trial_lifecycle = false;
    std::filesystem::path artifacts_directory;
    std::string run_fingerprint;
    std::size_t lifecycle_record_capacity = 0; // 0 = bounded auto sizing
};

inline void validate_mc_effective_config(const McRunConfig& config)
{
    const auto finite_nonnegative = [](double value) {
        return std::isfinite(value) && value >= 0.0;
    };
    if (config.instrument.symbol != config.generator_config.symbol
        || !(config.instrument.lot_size > 0.0)
        || !std::isfinite(config.instrument.lot_size)
        || !finite_nonnegative(config.instrument.tick_size)
        || !finite_nonnegative(config.instrument.min_qty)
        || !finite_nonnegative(config.instrument.min_notional))
        throw std::invalid_argument(
            "MC requires one complete matching synthetic instrument snapshot");
    const std::string_view fee_model = config.fee_model.empty()
        ? std::string_view{"zero"} : std::string_view{config.fee_model};
    if (fee_model != "zero" && fee_model != "fixed"
        && fee_model != "tiered")
        throw std::invalid_argument("MC fee model is unsupported");
    if (!finite_nonnegative(config.fee_value)
        || !finite_nonnegative(config.maker_rate)
        || !finite_nonnegative(config.taker_rate)
        || !finite_nonnegative(config.instrument.maker_rate)
        || !finite_nonnegative(config.instrument.taker_rate)
        || (fee_model == "fixed" && !(config.fee_value > 0.0)))
        throw std::invalid_argument(
            "MC fee schedule must be finite, non-negative, and complete");
    if (!finite_nonnegative(config.fill_probability)
        || config.fill_probability > 1.0
        || !finite_nonnegative(config.fill_fade)
        || config.fill_fade > 1.0
        || !finite_nonnegative(config.fill_decay)
        || (config.fill_fade > 0.0 && !(config.fill_probability > 0.0)))
        throw std::invalid_argument(
            "MC fill probability/fade/decay configuration is invalid");
    if (config.fill_probability == 0.0
        && (config.fill_fade != 0.0 || config.fill_decay != 10.0))
        throw std::invalid_argument(
            "MC disabled fill model refuses inert calibration");
    if (!finite_nonnegative(config.order_latency_us)
        || !finite_nonnegative(config.order_latency_stddev_us)
        || !finite_nonnegative(config.wire_latency_us))
        throw std::invalid_argument(
            "MC latency configuration must be finite and non-negative");
    if (config.order_latency_stddev_us > 0.0
        && !(config.order_latency_us > 0.0))
        throw std::invalid_argument(
            "MC stochastic latency requires a positive mean latency");
    if (config.wire_latency_us > 0.0)
        throw std::invalid_argument(
            "MC wire latency is unsupported by the current local execution adapter");
    if (!finite_nonnegative(config.impact_k_bps)
        || !finite_nonnegative(config.impact_adv)
        || ((config.impact_k_bps > 0.0) != (config.impact_adv > 0.0)))
        throw std::invalid_argument(
            "MC impact requires positive k_bps and ADV together");
    if (config.walked_book_impact)
        throw std::invalid_argument(
            "MC walked-book impact requires an L2-enabled trial consumer");
    if (!config.generator_config.emit_synthetic_l2
        && (config.generator_config.base_spread_bps != 4.0
            || config.generator_config.depth_noise != 0.15))
        throw std::invalid_argument(
            "MC synthetic-L2 calibration cannot be changed while L2 is disabled");
    if (config.mm_levels_per_side <= 0 || config.mm_base_depth <= 0
        || !(config.mm_spread_pct > 0.0)
        || !std::isfinite(config.mm_spread_pct)
        || !finite_nonnegative(config.mm_vol_spread_mult)
        || !(config.mm_max_half_spread_pct > 0.0)
        || !std::isfinite(config.mm_max_half_spread_pct))
        throw std::invalid_argument(
            "MC market-maker calibration is invalid");
    if (config.periods_per_year == 0 || config.rolling_window == 0
        || config.max_equity_points < 4
        || !std::isfinite(config.risk_free_rate))
        throw std::invalid_argument(
            "MC analytics configuration is invalid");
    if (!(config.initial_balance > 0.0)
        || !std::isfinite(config.initial_balance)
        || !finite_nonnegative(config.risk_fraction)
        || config.risk_fraction > 1.0 || config.sma_period == 0
        || !(config.market_aggression > 0.0)
        || !std::isfinite(config.market_aggression)
        || !(config.quantity_scale > 0.0)
        || !std::isfinite(config.quantity_scale))
        throw std::invalid_argument(
            "MC strategy/execution scaling configuration is invalid");
    if (!std::isfinite(config.exit_defaults.sl_pct)
        || !std::isfinite(config.exit_defaults.tp_pct)
        || !std::isfinite(config.exit_defaults.trail_pct)
        || config.exit_defaults.sl_pct < 0.0
        || config.exit_defaults.tp_pct < 0.0
        || config.exit_defaults.trail_pct < 0.0)
        throw std::invalid_argument(
            "MC exit configuration is invalid");
}

} // namespace truetest::simulation
