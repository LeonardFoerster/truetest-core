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
    TrialResult result;
    result.trial_id = trial_index;
    result.seed_used = derive_trial_seed(trial_index);

    // 1. Generate path for this trial
    SyntheticPath path = generator_->generate(result.seed_used, config_.generator_config);

    // 2. Fresh data handler + load generated bars
    auto dh = std::make_shared<data_handler>();
    load_synthetic_path_into_handler(path, dh);

    // 3. Fresh strategy instance
    auto strategy = StrategyRegistry::instance().create(config_.strategy_name);
    if (!strategy) {
        strategy = StrategyRegistry::instance().create("mean-reversion"); // safe fallback
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

    // 5. Construct and run a completely fresh engine
    engine eng(dh, nullptr, strategy, std::move(ecfg));

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

    // TODO: winning_trades extraction (AnalyticsReport has some win/loss data in sub-analytics)

    return result;
}

McAggregate MonteCarloController::run() {
    McAggregate aggregate;
    aggregate.n_trials = config_.n_trials;
    aggregate.trials.reserve(config_.n_trials);

    for (std::size_t i = 0; i < config_.n_trials; ++i) {
        TrialResult tr = run_single_trial(i);
        aggregate.trials.push_back(tr);

        // Running online stats (very cheap)
        // (we'll compute proper quantiles at the end for simplicity in Phase 2)
    }

    // Compute aggregate statistics
    if (!aggregate.trials.empty()) {
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
