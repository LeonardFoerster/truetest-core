#include "monte_carlo_controller.h"

#include "simulation/generators/gbm_generator.h"   // Phase 1: only GBM for now

// Engine & strategy includes (needed to run isolated trials)
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "strategy/strategy_registry.h"
#include "strategy/apply_execution_cost_params.h"
#include "execution/fee_model.h"
#include "execution/impact_model.h"
#include "execution/latency_model.h"
#include "execution/queue_model.h"
#include "analytics/analytics.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
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

void apply_strategy_params(IStrategy& strategy,
                           const std::vector<std::pair<std::string, double>>& params) {
    for (const auto& [key, value] : params) {
        strategy.set_param(key, value);
    }
}

// Push fee realism into strategy sizing params (shared with CLI main.inc).
// Without this, MC charges commissions on fills but fee-aware strategies size
// as if fees=0.
void apply_execution_cost_params(IStrategy& strategy, const McRunConfig& cfg) {
    ::apply_execution_cost_params(strategy, cfg.maker_rate, cfg.taker_rate,
                                  cfg.bar_spread_bps, cfg.fee_model, cfg.fee_value);
}

} // anonymous namespace

void summarize_monte_carlo_trials(McAggregate& aggregate) {
    aggregate.n_trials = aggregate.trials.size();
    aggregate.mean_pnl = 0.0;
    aggregate.median_pnl = 0.0;
    aggregate.p5_pnl = 0.0;
    aggregate.p95_pnl = 0.0;
    aggregate.mean_sharpe = 0.0;
    aggregate.median_sharpe = 0.0;
    aggregate.mean_max_dd = 0.0;
    aggregate.worst_max_dd = 0.0;
    aggregate.profit_factor_pooled = 0.0;
    aggregate.profit_factor_mean = 0.0;
    aggregate.median_profit_factor = 0.0;
    aggregate.profit_factor_mean_valid = 0.0;
    aggregate.median_profit_factor_valid = 0.0;
    aggregate.profit_factor_pooled_unbounded = false;
    aggregate.win_rate_mean = 0.0;
    aggregate.median_win_rate = 0.0;
    aggregate.trials_with_positive_pnl = 0;
    aggregate.trials_with_profit_factor_gt_1 = 0;
    aggregate.valid_profit_factor_trials = 0;
    aggregate.unbounded_profit_factor_trials = 0;

    if (aggregate.trials.empty()) return;

    std::vector<double> pnls;
    std::vector<double> sharpes;
    std::vector<double> maxdds;
    std::vector<double> win_rates;
    std::vector<double> profit_factors;
    std::vector<double> valid_profit_factors;
    pnls.reserve(aggregate.trials.size());
    sharpes.reserve(aggregate.trials.size());
    maxdds.reserve(aggregate.trials.size());
    win_rates.reserve(aggregate.trials.size());
    profit_factors.reserve(aggregate.trials.size());
    valid_profit_factors.reserve(aggregate.trials.size());

    double pooled_win = 0.0;
    double pooled_loss = 0.0;
    for (const auto& trial : aggregate.trials) {
        pnls.push_back(trial.total_pnl);
        sharpes.push_back(trial.sharpe_ratio);
        maxdds.push_back(trial.max_drawdown);
        win_rates.push_back(trial.win_rate);
        profit_factors.push_back(trial.profit_factor);
        pooled_win += trial.total_win;
        pooled_loss += trial.total_loss;

        if (trial.total_loss > 0.0) {
            valid_profit_factors.push_back(trial.total_win / trial.total_loss);
            ++aggregate.valid_profit_factor_trials;
        } else if (trial.total_win > 0.0) {
            ++aggregate.unbounded_profit_factor_trials;
        }
        if (trial.total_pnl > 0.0) ++aggregate.trials_with_positive_pnl;
        if (trial.profit_factor > 1.0)
            ++aggregate.trials_with_profit_factor_gt_1;
    }

    std::sort(pnls.begin(), pnls.end());
    std::sort(sharpes.begin(), sharpes.end());
    std::sort(maxdds.begin(), maxdds.end());
    std::sort(win_rates.begin(), win_rates.end());
    std::sort(profit_factors.begin(), profit_factors.end());
    std::sort(valid_profit_factors.begin(), valid_profit_factors.end());

    const auto mean = [](const std::vector<double>& values) {
        if (values.empty()) return 0.0;
        double sum = 0.0;
        for (const double value : values) sum += value;
        return sum / static_cast<double>(values.size());
    };

    const auto percentile_index = [](std::size_t size,
                                     std::size_t percentile) {
        return (size / 100U) * percentile +
               ((size % 100U) * percentile) / 100U;
    };

    aggregate.mean_pnl = mean(pnls);
    aggregate.median_pnl = pnls[pnls.size() / 2];
    aggregate.p5_pnl = pnls[percentile_index(pnls.size(), 5U)];
    aggregate.p95_pnl = pnls[percentile_index(pnls.size(), 95U)];

    aggregate.mean_sharpe = mean(sharpes);
    aggregate.median_sharpe = sharpes[sharpes.size() / 2];
    aggregate.mean_max_dd = mean(maxdds);
    aggregate.worst_max_dd = *std::max_element(maxdds.begin(), maxdds.end());
    aggregate.win_rate_mean = mean(win_rates);
    aggregate.median_win_rate = win_rates[win_rates.size() / 2];

    if (pooled_loss > 0.0) {
        aggregate.profit_factor_pooled = pooled_win / pooled_loss;
    } else if (pooled_win > 0.0) {
        aggregate.profit_factor_pooled = 1e9;
        aggregate.profit_factor_pooled_unbounded = true;
    }
    aggregate.profit_factor_mean = mean(profit_factors);
    aggregate.median_profit_factor = profit_factors[profit_factors.size() / 2];
    aggregate.profit_factor_mean_valid = mean(valid_profit_factors);
    if (!valid_profit_factors.empty()) {
        aggregate.median_profit_factor_valid =
            valid_profit_factors[valid_profit_factors.size() / 2];
    }
}

MonteCarloController::MonteCarloController(const McRunConfig& run_config)
    : config_(run_config)
{
    if (config_.parallel_trials && config_.reuse_objects_between_trials) {
        throw std::runtime_error(
            "MonteCarlo: parallel_trials and reuse_objects_between_trials are "
            "mutually exclusive (shared strategy/data_handler would race)");
    }
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

TrialResult MonteCarloController::run_single_trial_with_path(std::size_t trial_index, SyntheticPath path) {
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
        reusable_strategy_->reset(result.seed_used);   // pass seed for RNG-based strategies
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
        // Costs first, then --param overrides (same order as single-run CLI).
        apply_execution_cost_params(*strategy, config_);
        try {
            strategy->set_param("risk_fraction", config_.risk_fraction);
        } catch (const std::exception&) {
            // Not every strategy exposes equity-fraction sizing. This mirrors
            // the single-run CLI's capability-based platform default.
        }
        apply_strategy_params(*strategy, config_.strategy_params);
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
    ecfg.show_progress = false;       // MC campaigns: avoid progress spam

    if (config_.fee_model == "fixed")
        ecfg.fee_model = std::make_shared<FixedFeeModel>(config_.fee_value);
    else if (config_.fee_model == "tiered")
        ecfg.fee_model = std::make_shared<TieredFeeModel>(
            config_.maker_rate, config_.taker_rate);

    if (config_.order_latency_us > 0.0)
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

    // 6. Extract key metrics
    const auto& analytics = eng->get_analytics();
    auto report = analytics.snapshot();   // or generate_report()

    result.initial_equity = report.initial_equity;
    result.final_equity   = report.final_equity;
    result.total_pnl      = result.final_equity - result.initial_equity;
    result.max_drawdown   = report.max_drawdown;
    result.sharpe_ratio   = report.sharpe_ratio;
    result.total_trades    = report.total_trades;
    result.winning_trades  = report.winning_trades;
    result.win_rate        = report.win_rate;
    result.profit_factor   = report.profit_factor;
    result.total_win       = analytics.gross_profit();
    result.total_loss      = analytics.gross_loss();

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
        // Experimental parallel execution (Phase 5)
        std::cerr << "WARNING: Parallel MC trials enabled. This conflicts with engine core pinning, "
                  << "thread presets, and some realism models. Use only with --thread-preset inline. "
                  << "Result ordering and some timing statistics may not be fully deterministic.\n";

        unsigned hw = std::thread::hardware_concurrency();
        unsigned threads = config_.max_parallel_threads > 0 ? config_.max_parallel_threads : hw;
        if (threads == 0) threads = 4; // fallback

        std::mutex mtx;
        std::vector<std::jthread> workers;

        for (std::size_t i = 0; i < config_.n_trials; ++i) {
            workers.emplace_back([this, i, &paths, &aggregate, &mtx]() {
                try {
                    TrialResult tr = run_single_trial_with_path(i, std::move(paths[i]));
                    std::lock_guard<std::mutex> lock(mtx);
                    aggregate.trials.push_back(std::move(tr));
                } catch (const std::exception& ex) {
                    std::cerr << "WARNING: Trial " << i << " failed with exception: " << ex.what() << "\n";
                    // Push a failed result so aggregates remain consistent
                    TrialResult failed;
                    failed.trial_id = i;
                    failed.seed_used = derive_trial_seed(i);
                    std::lock_guard<std::mutex> lock(mtx);
                    aggregate.trials.push_back(std::move(failed));
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

        // Stable ordering for deterministic aggregates / report drill-down
        std::sort(aggregate.trials.begin(), aggregate.trials.end(),
                  [](const TrialResult& a, const TrialResult& b) {
                      return a.trial_id < b.trial_id;
                  });

    } else {
        for (std::size_t i = 0; i < config_.n_trials; ++i) {
            TrialResult tr = run_single_trial_with_path(i, std::move(paths[i]));
            aggregate.trials.push_back(tr);
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
