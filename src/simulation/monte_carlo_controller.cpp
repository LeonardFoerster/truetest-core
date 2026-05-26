#include "monte_carlo_controller.h"

#include "simulation/generators/gbm_generator.h"   // Phase 1: only GBM for now

// Engine & strategy includes (needed to run isolated trials)
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "strategy/strategy_registry.h"
#include "execution/fee_model.h"
#include "analytics/analytics.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
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
                                 bar.volume);
    }
    // Note: caller may need to sort if multi-symbol, but single symbol here
}

} // anonymous namespace

MonteCarloController::MonteCarloController(const McRunConfig& run_config)
    : config_(run_config)
{
    ensure_generator();
}

void MonteCarloController::ensure_generator() {
    if (generator_) return;

    // Phase 2: only GBM supported (easy to extend with factory later)
    if (config_.generator_name == "gbm" || config_.generator_name.empty()) {
        generator_ = std::make_unique<GBMGenerator>();
    } else {
        // Fallback to GBM for now
        generator_ = std::make_unique<GBMGenerator>();
    }
}

uint64_t MonteCarloController::derive_trial_seed(std::size_t trial_index) const {
    uint64_t base = config_.base_seed;
    if (base == 0) {
        // Stable default if user didn't provide one
        base = 0xA5A5C0DE42ULL;
    }
    // Good statistical mixing + reproducibility
    return base ^ (static_cast<uint64_t>(trial_index) * 0x9e3779b97f4a7c15ULL + 0x123456789ABCDEFULL);
}

TrialResult MonteCarloController::run_single_trial(std::size_t trial_index) {
    uint64_t seed = derive_trial_seed(trial_index);
    SyntheticPath path = generator_->generate(seed, config_.generator_config);
    return run_single_trial_with_path(trial_index, std::move(path));
}

TrialResult MonteCarloController::run_single_trial_with_path(std::size_t trial_index, SyntheticPath path) {
    TrialResult result;
    result.trial_id = trial_index;
    result.seed_used = derive_trial_seed(trial_index);

    // 1. (path already provided by caller / batch generation)

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

    // 3. Strategy (reuse or fresh)
    std::shared_ptr<IStrategy> strategy;
    if (config_.reuse_objects_between_trials && reusable_strategy_)
    {
        reusable_strategy_->reset(result.seed_used);   // pass seed for RNG-based strategies
        strategy = reusable_strategy_;
    }
    else
    {
        strategy = StrategyRegistry::instance().create(config_.strategy_name);
        if (!strategy) {
            strategy = StrategyRegistry::instance().create("mean-reversion");
        }
        if (config_.reuse_objects_between_trials)
            reusable_strategy_ = strategy;
    }

    // 4. Build minimal engine config for this trial
    engine_config ecfg;
    ecfg.mode = engine_mode::backtest;
    ecfg.initial_balance = 10000.0;   // TODO: make configurable in McRunConfig later
    ecfg.seed = result.seed_used;     // important for any internal RNGs

    // Apply realism settings from McRunConfig (Phase 2 keeps this lightweight)
    if (config_.realistic_fills) {
        ecfg.realistic_fills = true;
    }
    // Latency / impact can be wired here in later phases using the existing model classes
    // when McRunConfig exposes more detailed realism knobs.

    // 5. Construct engine (still fresh construction in Phase A)
    engine eng(dh, nullptr, strategy, std::move(ecfg));

    // If reuse requested, reset the heavy objects the engine owns so they can be
    // reused for the next trial (good-enough reuse, not bit-identical in all cases).
    if (config_.reuse_objects_between_trials)
    {
        eng.reset_for_next_trial(result.seed_used);
    }

    // Run the backtest
    if (!path.bars.empty()) {
        eng.run();
    } else if (!path.ticks.empty()) {
        eng.run_tick_data();
    }

    // 6. Extract key metrics
    const auto& analytics = eng.get_analytics();
    auto report = analytics.snapshot();   // or generate_report()

    result.initial_equity = report.initial_equity;
    result.final_equity   = report.final_equity;
    result.total_pnl      = result.final_equity - result.initial_equity;
    result.max_drawdown   = report.max_drawdown;
    result.sharpe_ratio   = report.sharpe_ratio;
    result.total_trades   = report.total_trades;

    // winning_trades can be extracted from sub-analytics in a future pass if needed.

    return result;
}

McAggregate MonteCarloController::run() {
    McAggregate aggregate;
    aggregate.n_trials = config_.n_trials;
    aggregate.trials.reserve(config_.n_trials);

    auto start = std::chrono::steady_clock::now();

    // Performance optimization (Phase 5): generate all paths in one batch first.
    // This is cache-friendly and allows the generator implementation to optimize
    // (future SIMD / vectorized path generation, prefetch, etc.).
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

    } else {
        for (std::size_t i = 0; i < config_.n_trials; ++i) {
            TrialResult tr = run_single_trial_with_path(i, std::move(paths[i]));
            aggregate.trials.push_back(tr);
        }
    }

    auto end = std::chrono::steady_clock::now();
    aggregate.wall_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    if (config_.reuse_objects_between_trials) {
        std::cout << "  [MC Reuse] Wall time with object reuse: " 
                  << aggregate.wall_time_ms << " ms\n";
    }

    // Compute aggregate statistics (always serial, after collection)
    if (!aggregate.trials.empty()) {
        // Recompute positive count safely
        aggregate.trials_with_positive_pnl = 0;

        std::vector<double> pnls;
        std::vector<double> sharpes;
        std::vector<double> maxdds;
        pnls.reserve(aggregate.trials.size());
        sharpes.reserve(aggregate.trials.size());
        maxdds.reserve(aggregate.trials.size());

        for (const auto& t : aggregate.trials) {
            pnls.push_back(t.total_pnl);
            sharpes.push_back(t.sharpe_ratio);
            maxdds.push_back(t.max_drawdown);

            if (t.total_pnl > 0.0) ++aggregate.trials_with_positive_pnl;
        }

        // Simple mean / median (Phase 2 — can upgrade to proper quantile later)
        std::sort(pnls.begin(), pnls.end());
        std::sort(sharpes.begin(), sharpes.end());
        std::sort(maxdds.begin(), maxdds.end());

        auto mean = [](const std::vector<double>& v) {
            if (v.empty()) return 0.0;
            double s = 0.0; for (double x : v) s += x; return s / v.size();
        };

        aggregate.mean_pnl     = mean(pnls);
        aggregate.median_pnl   = pnls[pnls.size() / 2];
        aggregate.p5_pnl       = pnls[static_cast<size_t>(pnls.size() * 0.05)];
        aggregate.p95_pnl      = pnls[static_cast<size_t>(pnls.size() * 0.95)];

        aggregate.mean_sharpe  = mean(sharpes);
        aggregate.median_sharpe = sharpes[sharpes.size() / 2];

        aggregate.mean_max_dd  = mean(maxdds);
        aggregate.worst_max_dd = *std::max_element(maxdds.begin(), maxdds.end());
    }

    return aggregate;
}

} // namespace truetest::simulation
