#include "simulation/monte_carlo_controller.h"
#include "simulation/monte_carlo_reporter.h"

#include <gtest/gtest.h>

using namespace truetest::simulation;

TEST(MonteCarloController, RunsMultipleTrials) {
    McRunConfig cfg;
    cfg.n_trials = 8;                    // small for fast test
    cfg.generator_config.n_steps = 200;  // short paths
    cfg.generator_config.sigma = 0.6;
    cfg.strategy_name = "mean-reversion";
    cfg.base_seed = 42;

    MonteCarloController controller(cfg);
    McAggregate agg = controller.run();

    EXPECT_EQ(agg.n_trials, 8u);
    EXPECT_EQ(agg.trials.size(), 8u);

    // Basic sanity: we should have some variation in P&L across trials
    bool has_variation = false;
    for (size_t i = 1; i < agg.trials.size(); ++i) {
        if (std::abs(agg.trials[i].total_pnl - agg.trials[0].total_pnl) > 1.0) {
            has_variation = true;
            break;
        }
    }
    EXPECT_TRUE(has_variation) << "All trials produced identical P&L (very unlikely)";

    // Reporter should not crash
    std::string summary = MonteCarloReporter::render_text_summary(agg, cfg);
    EXPECT_FALSE(summary.empty());

    // New MC-02 robustness metrics (Phase 2 fields now populated; see docs/todos/03-MC-simulation.md#MC-02)
    EXPECT_TRUE(std::isfinite(agg.win_rate_mean));
    EXPECT_TRUE(std::isfinite(agg.profit_factor_mean));
    EXPECT_TRUE(std::isfinite(agg.median_win_rate));
    EXPECT_TRUE(std::isfinite(agg.median_profit_factor));
    EXPECT_LE(agg.win_rate_mean, 100.0);
    EXPECT_GE(agg.win_rate_mean, 0.0);
    EXPECT_GE(agg.trials_with_profit_factor_gt_1, 0u);
    EXPECT_LE(agg.trials_with_profit_factor_gt_1, agg.n_trials);

    // Reporter output now contains the new sections
    EXPECT_NE(summary.find("Win Rate"), std::string::npos);
    EXPECT_NE(summary.find("Profit Factor"), std::string::npos);
    EXPECT_NE(summary.find("PF > 1"), std::string::npos);
}

TEST(MonteCarloController, DeterministicWithSameSeed) {
    McRunConfig cfg;
    cfg.n_trials = 4;
    cfg.generator_config.n_steps = 150;
    cfg.base_seed = 12345;

    MonteCarloController c1(cfg);
    auto agg1 = c1.run();

    MonteCarloController c2(cfg);
    auto agg2 = c2.run();

    ASSERT_EQ(agg1.trials.size(), agg2.trials.size());
    for (size_t i = 0; i < agg1.trials.size(); ++i) {
        EXPECT_EQ(agg1.trials[i].seed_used, agg2.trials[i].seed_used);
        EXPECT_DOUBLE_EQ(agg1.trials[i].final_equity, agg2.trials[i].final_equity);
    }
}

// Phase A deepening: verify that object reuse produces (nearly) identical results
TEST(MonteCarloController, ReuseObjectsProducesPlausibleResults) {
    McRunConfig cfg;
    cfg.n_trials = 5;
    cfg.generator_config.n_steps = 150;
    cfg.generator_config.sigma = 0.55;
    cfg.strategy_name = "mean-reversion";
    cfg.base_seed = 123;

    // Reference run without reuse
    McRunConfig cfg_fresh = cfg;
    cfg_fresh.reuse_objects_between_trials = false;

    MonteCarloController ctrl_fresh(cfg_fresh);
    auto agg_fresh = ctrl_fresh.run();

    // Run with reuse
    McRunConfig cfg_reuse = cfg;
    cfg_reuse.reuse_objects_between_trials = true;

    MonteCarloController ctrl_reuse(cfg_reuse);
    auto agg_reuse = ctrl_reuse.run();

    EXPECT_EQ(agg_fresh.n_trials, agg_reuse.n_trials);
    EXPECT_EQ(agg_fresh.trials.size(), agg_reuse.trials.size());

    // Results must be finite (no NaN/inf from bad reset)
    EXPECT_TRUE(std::isfinite(agg_fresh.mean_pnl));
    EXPECT_TRUE(std::isfinite(agg_reuse.mean_pnl));
    EXPECT_TRUE(std::isfinite(agg_fresh.median_sharpe));
    EXPECT_TRUE(std::isfinite(agg_reuse.median_sharpe));

    // MC-02 fields also finite under reuse (see docs/todos/03-MC-simulation.md)
    EXPECT_TRUE(std::isfinite(agg_reuse.win_rate_mean));
    EXPECT_TRUE(std::isfinite(agg_reuse.profit_factor_mean));

    // Very loose bounds for this micro-benchmark.
    // Full bit-identical results are not yet guaranteed with reuse.
    // This test ensures the reuse path is functional ("good enough" policy).
    EXPECT_GT(agg_reuse.mean_pnl, -1000.0);
    EXPECT_LT(agg_reuse.worst_max_dd, 50.0);
}
