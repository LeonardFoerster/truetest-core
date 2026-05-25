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
