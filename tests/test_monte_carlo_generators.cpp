#include "simulation/imonte_carlo_generator.h"
#include "simulation/generators/gbm_generator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace truetest::simulation;

TEST(GBMGenerator, ReproducibilitySameSeed) {
    GBMGenerator gen;
    McGeneratorConfig cfg = gen.default_config();
    cfg.n_steps = 200;
    cfg.seed = 0; // will be overridden by explicit seed

    const uint64_t seed = 0x123456789abcdef0ULL;

    auto p1 = gen.generate(seed, cfg);
    auto p2 = gen.generate(seed, cfg);

    ASSERT_EQ(p1.seed_used, p2.seed_used);
    ASSERT_EQ(p1.mids.size(), p2.mids.size());
    ASSERT_EQ(p1.bars.size(), p2.bars.size());

    for (size_t i = 0; i < p1.mids.size(); ++i) {
        EXPECT_DOUBLE_EQ(p1.mids[i], p2.mids[i]) << "Mismatch at step " << i;
    }
}

TEST(GBMGenerator, DifferentSeedsProduceDifferentPaths) {
    GBMGenerator gen;
    McGeneratorConfig cfg = gen.default_config();
    cfg.n_steps = 100;

    auto p1 = gen.generate(42, cfg);
    auto p2 = gen.generate(43, cfg);

    // Extremely unlikely to be identical for 100 steps
    bool all_equal = true;
    for (size_t i = 0; i < p1.mids.size(); ++i) {
        if (std::abs(p1.mids[i] - p2.mids[i]) > 1e-9) {
            all_equal = false;
            break;
        }
    }
    EXPECT_FALSE(all_equal);
}

TEST(GBMGenerator, PositivePricesAndReasonableReturns) {
    GBMGenerator gen;
    McGeneratorConfig cfg = gen.default_config();
    cfg.n_steps = 500;
    cfg.initial_price = 100.0;
    cfg.mu = 0.0;
    cfg.sigma = 0.8;   // high vol

    auto path = gen.generate(12345, cfg);

    ASSERT_FALSE(path.mids.empty());

    for (double p : path.mids) {
        EXPECT_GT(p, 0.0);
    }

    // Check that we have some movement (not all flat)
    double min_p = *std::min_element(path.mids.begin(), path.mids.end());
    double max_p = *std::max_element(path.mids.begin(), path.mids.end());
    EXPECT_GT(max_p / min_p, 1.01); // at least 1% range over 500 steps
}

TEST(GBMGenerator, BatchGenerationDeterministic) {
    GBMGenerator gen;
    McGeneratorConfig cfg = gen.default_config();
    cfg.n_steps = 50;

    auto batch1 = gen.generate_batch(5, 999, cfg);
    auto batch2 = gen.generate_batch(5, 999, cfg);

    ASSERT_EQ(batch1.size(), 5u);
    ASSERT_EQ(batch2.size(), 5u);

    for (size_t i = 0; i < 5; ++i) {
        ASSERT_EQ(batch1[i].mids.size(), batch2[i].mids.size());
        for (size_t j = 0; j < batch1[i].mids.size(); ++j) {
            EXPECT_DOUBLE_EQ(batch1[i].mids[j], batch2[i].mids[j]);
        }
    }
}

// Batch path seeds must use derive_mc_trial_seed (controller contract).
TEST(GBMGenerator, BatchUsesCanonicalTrialSeeds) {
    GBMGenerator gen;
    McGeneratorConfig cfg = gen.default_config();
    cfg.n_steps = 10;

    const uint64_t base = 42;
    auto batch = gen.generate_batch(4, base, cfg);
    ASSERT_EQ(batch.size(), 4u);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(batch[i].seed_used, derive_mc_trial_seed(base, i));
        // Explicit generate with that seed must match batch path
        auto single = gen.generate(derive_mc_trial_seed(base, i), cfg);
        ASSERT_EQ(single.mids.size(), batch[i].mids.size());
        for (size_t j = 0; j < single.mids.size(); ++j) {
            EXPECT_DOUBLE_EQ(single.mids[j], batch[i].mids[j]);
        }
    }

    // base_seed 0 → fixed default base
    auto batch0 = gen.generate_batch(2, 0, cfg);
    EXPECT_EQ(batch0[0].seed_used, derive_mc_trial_seed(0, 0));
    EXPECT_EQ(batch0[0].seed_used, kMcDefaultBaseSeed);
}

TEST(GBMGenerator, SyntheticPathContainsBarsAndTicks) {
    GBMGenerator gen;
    McGeneratorConfig cfg = gen.default_config();
    cfg.n_steps = 10;

    auto path = gen.generate(7, cfg);

    EXPECT_EQ(path.bars.size(), 10u);
    EXPECT_EQ(path.ticks.size(), 10u);
    EXPECT_EQ(path.mids.size(), 10u);
    EXPECT_EQ(path.symbol, cfg.symbol);
}
