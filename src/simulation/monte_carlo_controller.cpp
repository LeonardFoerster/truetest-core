#include "monte_carlo_controller.h"

#include "simulation/generators/gbm_generator.h"   // Phase 1: only GBM for now
#include "simulation/deterministic_mc_manifest.h"
#include "simulation/monte_carlo_aggregate.h"
#include "simulation/deterministic_trial_artifacts.h"

// Engine & strategy includes (needed to run isolated trials)
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "engine/deterministic_lifecycle_sink.h"
#include "engine/deterministic_order_id_scope.h"
#include "data/data_handler.h"
#include "strategy/strategy_registry.h"
#include "execution/fee_model.h"
#include "execution/impact_model.h"
#include "execution/latency_model.h"
#include "execution/queue_model.h"
#include "execution/fill_model_contract.h"
#include "analytics/analytics.h"
#include "reproducibility/deterministic_seed.h"
#include "reproducibility/run_manifest.h"
#include "reproducibility/sha256.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace truetest::simulation {

namespace {

// Helper: populate a data_handler from a SyntheticPath (bar mode for Phase 2)
void load_synthetic_path_into_handler(const SyntheticPath& path,
                                      std::shared_ptr<data_handler> handler) {
    if (!handler) return;

    // For Phase 2 we use the bar path (most common for MC)
    for (const auto& bar : path.bars) {
        // Reuse the existing sink logic pattern
        handler->load_into_queue(bar.date, bar.symbol,
                                 bar.open, bar.high, bar.low, bar.close,
                                 bar.volume, bar.quantity_scale);
    }
    // Note: caller may need to sort if multi-symbol, but single symbol here
}

} // anonymous namespace

MonteCarloController::MonteCarloController(const McRunConfig& run_config)
    : config_(run_config)
{
    if (!config_.master_seed_explicitly_set) {
        throw std::invalid_argument(
            "Monte Carlo requires an explicit deterministic master seed");
    }
    if (config_.parallel_trials && config_.reuse_objects_between_trials) {
        throw std::runtime_error(
            "MonteCarlo: parallel_trials and reuse_objects_between_trials are "
            "mutually exclusive (shared strategy/data_handler would race)");
    }
    if (config_.persist_trial_lifecycle) {
        if (config_.artifacts_directory.empty()) {
            throw std::invalid_argument(
                "deterministic Monte Carlo persistence requires an artifacts directory");
        }
        if (!reproducibility::is_lower_hex_sha256(config_.run_fingerprint)) {
            throw std::invalid_argument(
                "deterministic Monte Carlo persistence requires a valid run fingerprint");
        }
    }
    validate_mc_effective_config(config_);
    ensure_generator();
}

void MonteCarloController::ensure_generator() {
    if (generator_) return;

    // Phase 2: only GBM supported. Unknown names fail hard (no silent fallback).
    if (config_.generator_name == "gbm" || config_.generator_name.empty()) {
        generator_ = std::make_unique<GBMGenerator>();
    } else {
        throw std::runtime_error(
            "MonteCarlo: unknown generator '" + config_.generator_name +
            "' (only 'gbm' is supported)");
    }
}

uint64_t MonteCarloController::derive_trial_seed(std::size_t trial_index) const {
    return derive_mc_trial_seed(config_.base_seed, trial_index);
}

TrialResult MonteCarloController::run_single_trial(std::size_t trial_index) {
    uint64_t seed = derive_trial_seed(trial_index);
    SyntheticPath path = generator_->generate(seed, config_.generator_config);
    return run_single_trial_with_path(trial_index, std::move(path));
}

TrialResult MonteCarloController::run_trial(std::size_t trial_index)
{
    if (trial_index >= config_.n_trials)
        throw std::out_of_range(
            "MonteCarlo: requested trial index is outside the campaign");
    try
    {
        return run_single_trial(trial_index);
    }
    catch (const std::exception& error)
    {
        persist_failed_trial(trial_index, error.what());
        throw;
    }
    catch (...)
    {
        persist_failed_trial(trial_index, "unknown non-standard exception");
        throw;
    }
}

void MonteCarloController::persist_failed_trial(
    std::size_t trial_index, const std::string& error) const noexcept
{
    try
    {
        write_failed_trial_artifact(config_, trial_index, error);
    }
    catch (const std::exception& persistence_error)
    {
        std::cerr << "MonteCarlo: failed to persist failure status for trial "
                  << trial_index << ": " << persistence_error.what() << '\n';
    }
}

TrialResult MonteCarloController::run_single_trial_with_path(std::size_t trial_index, SyntheticPath path) {
    // Order ids are trial-local deterministic state. The RAII scope prevents
    // the process-global atomic sequence from making parallel trials depend on
    // worker scheduling and restores any outer test/application scope.
    truetest::engine_support::deterministic_order_id_scope order_ids;

    TrialResult result;
    result.trial_id = trial_index;
    // Single source of truth: same derive_mc_trial_seed used by generate_batch.
    result.seed_used = derive_trial_seed(trial_index);

    // Guard: path must have been generated with that seed (drill-down contract).
    const bool path_has_data =
        !path.bars.empty() || !path.ticks.empty() || !path.mids.empty();
    if (path_has_data && path.seed_used != result.seed_used) {
        throw std::runtime_error(
            "MonteCarlo: path.seed_used mismatch for trial " +
            std::to_string(trial_index) + " (path=" + std::to_string(path.seed_used) +
            ", expected=" + std::to_string(result.seed_used) +
            ") — seed derivation is inconsistent between generator and controller");
    }

    std::optional<TrialArtifactPaths> artifact_paths;
    std::shared_ptr<DeterministicLifecycleSink> lifecycle_sink;
    if (config_.persist_trial_lifecycle)
    {
        artifact_paths = trial_artifact_paths(config_, trial_index);
        prepare_trial_artifacts(*artifact_paths);

        if (config_.generator_config.emit_synthetic_l2)
        {
            if (path.l2_snapshots.size() != path.bars.size())
                throw std::runtime_error(
                    "MonteCarlo: synthetic L2 sidecar cardinality does not match the generated bar path");
            write_trial_synthetic_l2_partial(
                *artifact_paths, path.l2_snapshots);
        }

        if (config_.lifecycle_record_capacity == 0)
            throw std::invalid_argument(
                "deterministic Monte Carlo requires an explicit lifecycle record capacity");
        lifecycle_sink = std::make_shared<DeterministicLifecycleSink>(
            config_.lifecycle_record_capacity);
    }

    // 2. Data handler (reuse or fresh)
    std::shared_ptr<data_handler> dh;
    if (config_.reuse_objects_between_trials)
    {
        if (!reusable_data_handler_)
            reusable_data_handler_ = std::make_shared<data_handler>();
        else
            reusable_data_handler_->reset();
        dh = reusable_data_handler_;
    }
    else
    {
        dh = std::make_shared<data_handler>();
    }
    load_synthetic_path_into_handler(path, dh);

    // 3. Strategy (reuse or fresh). HIGH-03: refuse reuse for strategies that
    // do not fully clear trial-local state (supports_mc_trial_reuse() == false).
    std::shared_ptr<IStrategy> strategy;
    if (config_.reuse_objects_between_trials && reusable_strategy_)
    {
        if (!reusable_strategy_->supports_mc_trial_reuse()) {
            throw std::runtime_error(
                "MonteCarlo: strategy '" + config_.strategy_name +
                "' does not support mc-reuse-objects (incomplete reset)");
        }
        const auto strategy_seed = reproducibility::DeterministicSeedDeriver(
            config_.base_seed).trial_component_seed(
                static_cast<std::uint64_t>(trial_index),
                reproducibility::SeedDomain::strategy);
        reusable_strategy_->reset(strategy_seed);
        strategy = reusable_strategy_;
    }
    else
    {
        strategy = StrategyRegistry::instance().create(config_.strategy_name);
        if (!strategy) {
            throw std::runtime_error(
                "MonteCarlo: unknown strategy '" + config_.strategy_name +
                "' (no silent fallback)");
        }
        configure_deterministic_mc_strategy(*strategy, config_);
        const auto strategy_seed = reproducibility::DeterministicSeedDeriver(
            config_.base_seed).trial_component_seed(
                static_cast<std::uint64_t>(trial_index),
                reproducibility::SeedDomain::strategy);
        strategy->reset(strategy_seed);
        if (config_.reuse_objects_between_trials)
        {
            if (!strategy->supports_mc_trial_reuse()) {
                throw std::runtime_error(
                    "MonteCarlo: strategy '" + config_.strategy_name +
                    "' does not support mc-reuse-objects (incomplete reset); "
                    "use a strategy that implements supports_mc_trial_reuse()");
            }
            reusable_strategy_ = strategy;
        }
    }

    // 4. Build engine config for this trial — wire fee/latency/impact (FR-02).
    engine_config ecfg;
    ecfg.mode = engine_mode::backtest;
    ecfg.initial_balance = config_.initial_balance;
    ecfg.seed = result.seed_used;     // important for any internal RNGs
    ecfg.seed_explicitly_set = true;
    ecfg.show_progress = false;       // MC campaigns: avoid progress spam
    ecfg.rolling_window = config_.rolling_window;
    ecfg.risk_free_rate = config_.risk_free_rate;
    ecfg.periods_per_year = config_.periods_per_year;
    ecfg.max_equity_points = config_.max_equity_points;
    ecfg.execution_bar_delay = config_.execution_bar_delay;
    ecfg.market_aggression = config_.market_aggression;
    ecfg.qty_scale = config_.quantity_scale;
    ecfg.exit_defaults = config_.exit_defaults;
    ecfg.risk = config_.risk;
    ecfg.risk_unwind = config_.risk_unwind;
    ecfg.risk_soft_portfolio_limits = config_.risk_soft_portfolio_limits;
    ecfg.mm_levels_per_side = config_.mm_levels_per_side;
    ecfg.mm_base_depth = config_.mm_base_depth;
    ecfg.mm_base_spread_pct = config_.mm_spread_pct;
    ecfg.mm_vol_spread_mult = config_.mm_vol_spread_mult;
    ecfg.mm_max_half_spread_pct = config_.mm_max_half_spread_pct;
    ecfg.instrument_overrides.emplace(
        config_.instrument.symbol, config_.instrument);
    if (artifact_paths)
    {
        // Full deterministic persistence uses the existing logging worker.
        // Its block-on-full SPSC ring keeps every event while serialization,
        // compression, and file I/O stay outside the engine event loop.
        ecfg.threading = thread_preset::logging_only;
        ecfg.disable_pinning = true;
        ecfg.drop_policy = ring_drop_policy::block;
        ecfg.event_log_path = artifact_paths->event_log_partial.string();
        ecfg.compress_log = true;
        ecfg.log_max_bytes = 0;
        ecfg.log_max_files = 1;
        ecfg.order_audit_sink = lifecycle_sink;
    }

    if (config_.fee_model == "fixed")
        ecfg.fee_model = std::make_shared<FixedFeeModel>(config_.fee_value);
    else if (config_.fee_model == "tiered")
        ecfg.fee_model = std::make_shared<TieredFeeModel>(
            config_.maker_rate, config_.taker_rate);

    if (config_.fill_probability > 0.0)
        ecfg.fill_model = std::make_shared<RealisticFillModel>(
            config_.fill_fade, config_.fill_probability,
            config_.fill_decay);

    const auto latency_seed = reproducibility::DeterministicSeedDeriver(
        config_.base_seed).trial_component_seed(
            static_cast<std::uint64_t>(trial_index),
            reproducibility::SeedDomain::latency_model);
    if (config_.order_latency_us > 0.0
        && config_.order_latency_stddev_us > 0.0)
    {
        ecfg.latency_model = std::make_shared<StochasticLatencyModel>(
            config_.order_latency_us,
            config_.order_latency_stddev_us,
            latency_seed);
    }
    else if (config_.order_latency_us > 0.0)
    {
        ecfg.latency_model = std::make_shared<FixedLatencyModel>(
            latency_duration(static_cast<long long>(config_.order_latency_us)));
    }
    if (config_.impact_k_bps > 0.0 && config_.impact_adv > 0.0)
    {
        ecfg.impact_model = std::make_shared<SquareRootImpactModel>(
            config_.impact_k_bps, config_.impact_adv);
    }
    else if (config_.impact_k_bps > 0.0 && !(config_.impact_adv > 0.0))
    {
        // Refuse silent no-op: impact_k without ADV cannot be applied.
        throw std::runtime_error(
            "MonteCarlo: impact_k_bps > 0 requires impact_adv > 0 "
            "(would otherwise ignore impact knobs silently)");
    }

    // FR-mc-queue-unwired: wire maker-queue + walked-book same as single-run CLI.
    ecfg.walked_book_impact = config_.walked_book_impact;
    if (!config_.maker_queue_model.empty() && config_.maker_queue_model != "none")
    {
        if (config_.maker_queue_model == "uniform")
            ecfg.maker_queue_model = std::make_shared<UniformCancelModel>();
        else if (config_.maker_queue_model == "front")
            ecfg.maker_queue_model = std::make_shared<FrontCancelModel>();
        else if (config_.maker_queue_model == "back")
            ecfg.maker_queue_model = std::make_shared<BackCancelModel>();
        else
        {
            throw std::runtime_error(
                "MonteCarlo: unknown maker_queue_model '" + config_.maker_queue_model
                + "' (expected none|uniform|front|back)");
        }
    }

    // 5. Fresh engine per trial (not reset_for_next_trial). Engine reuse is
    // incomplete: adapters/pending/marks are not fully cleared by reset yet.
    // reuse_objects_between_trials only reuses data_handler + strategy.
    // engine embeds multi-MiB ObjectPools — never stack-allocate (8 MiB default stack).
    auto eng = std::make_unique<engine>(dh, nullptr, strategy, std::move(ecfg));
    // Stamp primary so order_meta / fill attribution carry strategy_name
    // (dispatch also falls back when empty; this keeps multi-strategy routing
    // and portfolio/audit attribution consistent).
    eng->set_primary_strategy_name(config_.strategy_name);

    // Run the backtest
    if (!path.bars.empty()) {
        eng->run();
    } else if (!path.ticks.empty()) {
        eng->run_tick_data();
    }
    if (!eng->run_succeeded())
        throw std::runtime_error(
            "MonteCarlo: engine halted before deterministic trial completion");

    // 6. Extract key metrics
    const auto& analytics = eng->get_analytics();
    auto report = analytics.snapshot();   // or generate_report()
    if (!report.accounting_reconciled)
        throw std::runtime_error(
            "MonteCarlo trial produced unreconciled accounting: "
            + report.accounting_reconciliation_reason);

    result.initial_equity = report.initial_equity;
    result.final_equity   = report.final_equity;
    result.total_pnl      = result.final_equity - result.initial_equity;
    result.max_drawdown   = report.max_drawdown;
    result.sharpe_ratio   = report.sharpe_ratio;
    result.sharpe_ratio_valid = report.sharpe_ratio_valid;
    result.accounting_reconciled = true;
    result.total_trades    = report.total_trades;
    result.winning_trades  = report.winning_trades;
    result.win_rate        = report.win_rate;
    result.profit_factor   = report.profit_factor;
    result.profit_factor_valid = report.profit_factor_valid;
    result.profit_factor_unbounded = report.profit_factor_unbounded;
    result.total_win       = analytics.gross_profit();
    result.total_loss      = analytics.gross_loss();

    if (artifact_paths)
    {
        if (!lifecycle_sink || lifecycle_sink->overflowed())
            throw std::runtime_error(
                "MonteCarlo: deterministic lifecycle capture overflowed");

        // Engine owns the logger. Destruction closes the staging file before
        // its atomic rename into the stable trial artifact name.
        eng.reset();
        finalize_trial_event_log(*artifact_paths);
        result.event_log_sha256 = reproducibility::sha256_file_hex(
            artifact_paths->event_log);
        result.lifecycle_sha256 = lifecycle_sink->write_atomic_and_hash(
            artifact_paths->lifecycle);
        if (config_.generator_config.emit_synthetic_l2)
        {
            finalize_trial_synthetic_l2(*artifact_paths);
            result.synthetic_l2_sha256 = reproducibility::sha256_file_hex(
                artifact_paths->synthetic_l2);
        }
        result.trial_result_sha256 = trial_result_sha256(result);
        write_completed_trial_artifacts(
            config_, *artifact_paths, result);
    }

    return result;
}

McAggregate MonteCarloController::run() {
    if (config_.parallel_trials && config_.reuse_objects_between_trials) {
        throw std::runtime_error(
            "MonteCarlo: parallel_trials and reuse_objects_between_trials are "
            "mutually exclusive (shared strategy/data_handler would race)");
    }

    McAggregate aggregate;
    aggregate.n_trials = config_.n_trials;
    aggregate.trials.reserve(config_.n_trials);

    auto start = std::chrono::steady_clock::now();

    // Performance optimization (Phase 5): generate all paths in one batch first.
    // Uses the same derive_mc_trial_seed(base_seed, i) as run_single_trial.
    auto paths = generator_->generate_batch(config_.n_trials, config_.base_seed, config_.generator_config);

    if (config_.parallel_trials && config_.n_trials > 1) {
        std::cerr << "Parallel MC trials enabled: trial seeds, artifact names, "
                     "and reductions remain stable by trial index. Full "
                     "persistence adds one unpinned logging worker per active "
                     "trial as recorded in the manifest.\n";

        unsigned hw = std::thread::hardware_concurrency();
        unsigned threads = config_.max_parallel_threads > 0 ? config_.max_parallel_threads : hw;
        if (threads == 0) threads = 4; // fallback

        std::vector<std::jthread> workers;
        std::vector<std::optional<TrialResult>> trial_slots(config_.n_trials);
        std::vector<std::exception_ptr> trial_errors(config_.n_trials);

        for (std::size_t i = 0; i < config_.n_trials; ++i) {
            workers.emplace_back([this, i, &paths, &trial_slots, &trial_errors]() {
                try {
                    trial_slots[i] = run_single_trial_with_path(
                        i, std::move(paths[i]));
                } catch (const std::exception& error) {
                    persist_failed_trial(i, error.what());
                    trial_errors[i] = std::current_exception();
                } catch (...) {
                    persist_failed_trial(i, "unknown non-standard exception");
                    trial_errors[i] = std::current_exception();
                }
            });

            // Simple throttle if we have more workers than threads
            if (workers.size() >= threads) {
                workers.front().join();
                workers.erase(workers.begin());
            }
        }

        // Wait for remaining
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        // Publish strictly in stable trial-index order. Completion order is
        // never observable by the reducer, reporter, or artifact writer.
        for (std::size_t i = 0; i < config_.n_trials; ++i)
        {
            if (trial_errors[i])
                std::rethrow_exception(trial_errors[i]);
            if (!trial_slots[i])
                throw std::runtime_error(
                    "MonteCarlo: trial completed without a result");
            aggregate.trials.push_back(std::move(*trial_slots[i]));
        }

    } else {
        for (std::size_t i = 0; i < config_.n_trials; ++i) {
            try
            {
                TrialResult tr = run_single_trial_with_path(
                    i, std::move(paths[i]));
                aggregate.trials.push_back(std::move(tr));
            }
            catch (const std::exception& error)
            {
                persist_failed_trial(i, error.what());
                throw;
            }
            catch (...)
            {
                persist_failed_trial(i, "unknown non-standard exception");
                throw;
            }
        }
    }

    auto end = std::chrono::steady_clock::now();
    aggregate.wall_time_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

    if (config_.reuse_objects_between_trials) {
        std::cout << "  [MC Reuse] Wall time with object reuse: " 
                  << aggregate.wall_time_ms << " ms\n";
    }

    // Compute aggregate statistics serially after the parallel path has been
    // restored to canonical trial_id order.
    summarize_monte_carlo_trials(aggregate);

    return aggregate;
}

} // namespace truetest::simulation
