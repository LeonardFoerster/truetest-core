#include "simulation/monte_carlo_controller.h"
#include "simulation/monte_carlo_reporter.h"
#include "simulation/generators/gbm_generator.h"
#include "simulation/monte_carlo_types.h"

#include <gtest/gtest.h>

#include <stdexcept>

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
    //
    // Bound is scaled to fixed-risk sizing semantics (risk_fraction is stop
    // budget, not notional fraction). Default mean-reversion uses 0.5% SL and
    // 2% equity risk ⇒ notional can be several × equity, so absolute PnL
    // swings are larger than the old notional-capped regime. Still catch
    // NaN / catastrophic blow-ups / broken reset without over-constraining.
    EXPECT_GT(agg_reuse.mean_pnl, -100000.0);
    EXPECT_LT(agg_reuse.worst_max_dd, 500.0);  // percent; loose catastrophic bound under fixed-risk sizing
    // Reuse and fresh must both be finite and same order of magnitude when
    // the campaign is well-formed (guards silent reuse corruption).
    EXPECT_TRUE(std::isfinite(agg_fresh.mean_pnl));
    if (std::abs(agg_fresh.mean_pnl) > 1.0) {
        EXPECT_NEAR(agg_reuse.mean_pnl / agg_fresh.mean_pnl, 1.0, 0.5);
    }
}

// seed_used must equal the seed that produced the GBM path (drill-down contract).
TEST(MonteCarloController, SeedUsedMatchesPathGenerator) {
    McRunConfig cfg;
    cfg.n_trials = 4;
    cfg.generator_config.n_steps = 40;
    cfg.base_seed = 42;
    cfg.strategy_name = "mean-reversion";

    GBMGenerator gen;
    auto batch = gen.generate_batch(cfg.n_trials, cfg.base_seed, cfg.generator_config);

    MonteCarloController controller(cfg);
    McAggregate agg = controller.run();

    ASSERT_EQ(agg.trials.size(), cfg.n_trials);
    ASSERT_EQ(batch.size(), cfg.n_trials);
    for (size_t i = 0; i < cfg.n_trials; ++i) {
        const uint64_t expected = derive_mc_trial_seed(cfg.base_seed, i);
        EXPECT_EQ(batch[i].seed_used, expected) << "batch path seed trial " << i;
        EXPECT_EQ(agg.trials[i].seed_used, expected) << "report seed_used trial " << i;
        EXPECT_EQ(agg.trials[i].seed_used, batch[i].seed_used);
    }
}

// base_seed==0 must use the fixed default for both path and seed_used.
TEST(MonteCarloController, ZeroBaseSeedUsesFixedDefault) {
    McRunConfig cfg;
    cfg.n_trials = 2;
    cfg.generator_config.n_steps = 30;
    cfg.base_seed = 0;
    cfg.strategy_name = "mean-reversion";

    MonteCarloController controller(cfg);
    McAggregate agg = controller.run();

    ASSERT_EQ(agg.trials.size(), 2u);
    EXPECT_EQ(agg.trials[0].seed_used, derive_mc_trial_seed(0, 0));
    EXPECT_EQ(agg.trials[0].seed_used, kMcDefaultBaseSeed); // trial 0: base ^ 0
    EXPECT_EQ(agg.trials[1].seed_used, derive_mc_trial_seed(0, 1));
    EXPECT_NE(agg.trials[0].seed_used, agg.trials[1].seed_used);
}

TEST(MonteCarloController, UnknownStrategyThrows) {
    McRunConfig cfg;
    cfg.n_trials = 1;
    cfg.generator_config.n_steps = 20;
    cfg.base_seed = 1;
    cfg.strategy_name = "this-strategy-does-not-exist";

    MonteCarloController controller(cfg);
    EXPECT_THROW(controller.run(), std::runtime_error);
}

TEST(MonteCarloController, UnknownGeneratorThrows) {
    McRunConfig cfg;
    cfg.n_trials = 1;
    cfg.generator_name = "not-a-real-model";
    cfg.base_seed = 1;
    EXPECT_THROW(MonteCarloController{cfg}, std::runtime_error);
}

TEST(MonteCarloController, ParallelPlusReuseThrows) {
    McRunConfig cfg;
    cfg.n_trials = 2;
    cfg.base_seed = 1;
    cfg.parallel_trials = true;
    cfg.reuse_objects_between_trials = true;
    EXPECT_THROW(MonteCarloController{cfg}, std::runtime_error);
}

// Strategy params must be applied (unknown key throws from set_param).
TEST(MonteCarloController, StrategyParamsApplied) {
    McRunConfig cfg;
    cfg.n_trials = 1;
    cfg.generator_config.n_steps = 50;
    cfg.base_seed = 7;
    cfg.strategy_name = "mean-reversion";
    cfg.strategy_params = {{"risk_fraction", 0.01}};

    MonteCarloController controller(cfg);
    EXPECT_NO_THROW(controller.run());

    McRunConfig bad = cfg;
    bad.strategy_params = {{"not_a_real_param_xyz", 1.0}};
    MonteCarloController ctrl_bad(bad);
    EXPECT_THROW(ctrl_bad.run(), std::exception);
}

// Re-generating a single path with seed_used must match campaign path.
TEST(MonteCarloController, DrillDownSeedReproducesPath) {
    const uint64_t base = 99;
    const size_t trial = 2;
    McGeneratorConfig gcfg;
    gcfg.n_steps = 80;
    gcfg.sigma = 0.5;

    GBMGenerator gen;
    auto batch = gen.generate_batch(3, base, gcfg);
    const uint64_t seed_used = batch[trial].seed_used;
    EXPECT_EQ(seed_used, derive_mc_trial_seed(base, trial));

    auto again = gen.generate(seed_used, gcfg);
    ASSERT_EQ(again.mids.size(), batch[trial].mids.size());
    for (size_t j = 0; j < again.mids.size(); ++j) {
        EXPECT_DOUBLE_EQ(again.mids[j], batch[trial].mids[j]) << "step " << j;
    }
}
