#include "simulation/imonte_carlo_generator.h"
#include "simulation/generators/gbm_generator.h"
#include "providers/synthetic/synthetic_provider.h"
#include "providers/synthetic/synthetic_transport.h"
#include "providers/local/csv_parser.h"
#include "engine/engine_config.h"
#include "reproducibility/deterministic_seed.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace truetest::simulation;

TEST(GBMGenerator, InvalidFinancialAndCapacityParametersFailClosed)
{
    GBMGenerator generator;
    const auto rejects = [&](auto mutate) {
        auto config = generator.default_config();
        mutate(config);
        EXPECT_THROW((void)generator.generate(42, config),
                     std::invalid_argument);
    };

    rejects([](auto& c) { c.symbol.clear(); });
    rejects([](auto& c) { c.symbol = "BTC,USDT"; });
    rejects([](auto& c) { c.symbol = "BTC USDT"; });
    rejects([](auto& c) { c.initial_price = 0.0; });
    rejects([](auto& c) {
        c.initial_price = std::numeric_limits<double>::infinity();
    });
    rejects([](auto& c) { c.n_steps = 0; });
    rejects([](auto& c) { c.n_steps = kMaxMcGeneratorSteps + 1; });
    rejects([](auto& c) { c.dt = -1.0; });
    rejects([](auto& c) {
        c.mu = std::numeric_limits<double>::quiet_NaN();
    });
    rejects([](auto& c) { c.sigma = -0.1; });
    rejects([](auto& c) {
        c.base_spread_bps = std::numeric_limits<double>::infinity();
    });
    rejects([](auto& c) { c.depth_noise = -0.1; });

    auto overflowing = generator.default_config();
    overflowing.n_steps = 1;
    overflowing.dt = 1.0;
    overflowing.mu = std::numeric_limits<double>::max();
    EXPECT_THROW((void)generator.generate(42, overflowing),
                 std::runtime_error);

    auto invalid_spread = generator.default_config();
    invalid_spread.n_steps = 1;
    invalid_spread.emit_synthetic_l2 = true;
    invalid_spread.base_spread_bps = std::numeric_limits<double>::max();
    EXPECT_THROW((void)generator.generate(42, invalid_spread),
                 std::runtime_error);

    auto invalid_depth = generator.default_config();
    invalid_depth.n_steps = 1;
    invalid_depth.emit_synthetic_l2 = true;
    invalid_depth.depth_noise = std::numeric_limits<double>::max();
    EXPECT_THROW((void)generator.generate(42, invalid_depth),
                 std::runtime_error);
}

TEST(SyntheticProvider, ConfigParsingRejectsPartialUnknownAndNonFiniteValues)
{
    const auto rejects = [](provider_config config) {
        EXPECT_THROW({ SyntheticProvider provider(config); },
                     std::invalid_argument);
    };

    rejects({{"sigma", "0.4junk"}});
    rejects({{"initial_price", "nan"}});
    rejects({{"n_steps", "0"}});
    rejects({{"seed", "-1"}});
    rejects({{"emit_l2", "yes"}});
    rejects({{"mc_params", "mu=0.1,unknown=2"}});
    rejects({{"mc_params", "mu=0.1,mu=0.2"}});
    rejects({{"mc_params", "n_steps=10,n_bars=20"}});
    rejects({{"sigma", "0.4"}, {"mc_params", "sigma=0.5"}});
    rejects({{"symbol", " "}});
    rejects({{"symbol", "BTC,USDT"}});
    rejects({{"emit_l2", "true"}});
    rejects({{"mc_params", "mu=0.1,"}});
    rejects({{"n_steps", "10"}, {"n_bars", "10"}});
    rejects({{"mc_params", "mu=0.1"}, {"params", "sigma=0.2"}});
}

TEST(SyntheticProvider, ZeroVolatilityIsPreservedAsAValidDeterministicModel)
{
    provider_config config{{"sigma", "0"}, {"n_steps", "10"}};
    SyntheticProvider provider(config);
    engine_config engine;
    engine.seed = 42;
    engine.seed_explicitly_set = true;
    provider.configure(engine);
    EXPECT_TRUE(provider.open());
    EXPECT_NE(provider.get_transport(), nullptr);
    provider.close();
}

TEST(SyntheticProvider, MasterSeedDomainDerivationDrivesTheMarketPath)
{
    const auto lines = [](SyntheticProvider& provider) {
        std::vector<std::string> result;
        EXPECT_TRUE(provider.open());
        while (auto line = provider.get_transport()->read_line())
            result.push_back(std::move(*line));
        provider.close();
        return result;
    };

    provider_config inherited_config{{"n_steps", "5"}};
    SyntheticProvider inherited(inherited_config);
    engine_config inherited_engine;
    inherited_engine.seed = 12345;
    inherited_engine.seed_explicitly_set = true;
    inherited.configure(inherited_engine);

    SyntheticProvider same_master(inherited_config);
    same_master.configure(inherited_engine);
    EXPECT_EQ(lines(inherited), lines(same_master));

    SyntheticProvider zero_master(inherited_config);
    engine_config zero_engine;
    zero_engine.seed = 0;
    zero_engine.seed_explicitly_set = true;
    zero_master.configure(zero_engine);

    SyntheticProvider different_master(inherited_config);
    engine_config different_engine;
    different_engine.seed = 999;
    different_engine.seed_explicitly_set = true;
    different_master.configure(different_engine);
    EXPECT_NE(lines(zero_master), lines(different_master));
}

TEST(SyntheticProvider, ProviderLocalOrMissingSeedFailsClosedBeforeGeneration)
{
    EXPECT_THROW((void)SyntheticProvider(provider_config{
                     {"n_steps", "5"}, {"seed", "12345"}}),
                 std::invalid_argument);
    EXPECT_THROW((void)SyntheticProvider(provider_config{
                     {"n_steps", "5"}, {"mc_params", "seed=12345"}}),
                 std::invalid_argument);

    SyntheticProvider provider(provider_config{{"n_steps", "5"}});
    EXPECT_THROW((void)provider.open(), std::logic_error);
    engine_config implicit;
    implicit.seed = 12345;
    EXPECT_THROW(provider.configure(implicit), std::invalid_argument);
}

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

TEST(GBMGenerator, BatchCapacityFailsClosedBeforeAllocation)
{
    GBMGenerator generator;
    auto config = generator.default_config();
    config.n_steps = 101;

    EXPECT_THROW((void)generator.generate_batch(0, 42, config),
                 std::invalid_argument);
    EXPECT_THROW((void)generator.generate_batch(
                     kMaxMcGeneratorPaths + 1, 42, config),
                 std::invalid_argument);
    EXPECT_THROW((void)generator.generate_batch(
                     kMaxMcGeneratorPaths, 42, config),
                 std::invalid_argument);

    config.n_steps = 1;
    config.emit_synthetic_l2 = true;
    const auto l2_batch = generator.generate_batch(1, 42, config);
    ASSERT_EQ(l2_batch.size(), 1U);
    ASSERT_EQ(l2_batch.front().l2_snapshots.size(), 1U);
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

    // An explicitly supplied zero is a normal master seed, not a fallback.
    auto batch0 = gen.generate_batch(2, 0, cfg);
    EXPECT_EQ(batch0[0].seed_used, derive_mc_trial_seed(0, 0));
    EXPECT_NE(batch0[0].seed_used, 0U);
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

TEST(GBMGenerator, BarsAndTicksShareTheSameCausalTimestamps)
{
    GBMGenerator generator;
    auto config = generator.default_config();
    config.n_steps = 20;
    const auto path = generator.generate(42, config);

    ASSERT_EQ(path.bars.size(), path.ticks.size());
    std::optional<std::int64_t> previous_timestamp_ms;
    for (std::size_t index = 0; index < path.bars.size(); ++index)
    {
        std::int64_t bar_timestamp_ms = 0;
        const std::string& encoded = path.bars[index].date;
        const auto [end, error] = std::from_chars(
            encoded.data(), encoded.data() + encoded.size(),
            bar_timestamp_ms);
        ASSERT_EQ(error, std::errc{});
        ASSERT_EQ(end, encoded.data() + encoded.size());
        const auto tick_timestamp_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                path.ticks[index].timestamp.time_since_epoch()).count();
        EXPECT_EQ(bar_timestamp_ms, tick_timestamp_ms);
        if (previous_timestamp_ms)
        {
            EXPECT_EQ(bar_timestamp_ms - *previous_timestamp_ms, 60'000);
        }
        previous_timestamp_ms = bar_timestamp_ms;
    }
}

TEST(GBMGenerator, ModelTimeDeterminesCausalEventSpacing)
{
    GBMGenerator generator;
    auto config = generator.default_config();
    config.n_steps = 3;
    config.dt = 1.0 / 365.0;
    config.mu = 0.0;
    config.sigma = 0.0;

    const auto path = generator.generate(42, config);
    ASSERT_EQ(path.ticks.size(), 3u);
    const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
        path.ticks[1].timestamp - path.ticks[0].timestamp).count();
    EXPECT_EQ(interval, 24LL * 60LL * 60LL * 1'000LL);
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::milliseconds>(
                  path.ticks[2].timestamp - path.ticks[1].timestamp).count(),
              interval);
}

TEST(GBMGenerator, OptionalL2DoesNotPerturbTheEconomicPricePath)
{
    GBMGenerator generator;
    auto without_l2 = generator.default_config();
    without_l2.n_steps = 200;
    auto with_l2 = without_l2;
    with_l2.emit_synthetic_l2 = true;

    const auto baseline = generator.generate(0xabcdef, without_l2);
    const auto enriched = generator.generate(0xabcdef, with_l2);
    ASSERT_EQ(enriched.mids.size(), baseline.mids.size());
    ASSERT_EQ(enriched.bars.size(), baseline.bars.size());
    ASSERT_EQ(enriched.ticks.size(), baseline.ticks.size());
    ASSERT_EQ(enriched.l2_snapshots.size(), enriched.mids.size());
    for (std::size_t i = 0; i < baseline.mids.size(); ++i)
    {
        EXPECT_DOUBLE_EQ(enriched.mids[i], baseline.mids[i]);
        EXPECT_DOUBLE_EQ(enriched.bars[i].open, baseline.bars[i].open);
        EXPECT_DOUBLE_EQ(enriched.bars[i].high, baseline.bars[i].high);
        EXPECT_DOUBLE_EQ(enriched.bars[i].low, baseline.bars[i].low);
        EXPECT_DOUBLE_EQ(enriched.bars[i].close, baseline.bars[i].close);
        EXPECT_EQ(enriched.bars[i].volume, baseline.bars[i].volume);
        EXPECT_DOUBLE_EQ(enriched.ticks[i].price, baseline.ticks[i].price);
        EXPECT_EQ(enriched.ticks[i].quantity, baseline.ticks[i].quantity);
    }
}

TEST(SyntheticTransport, CsvRoundTripPreservesGeneratedEconomicValuesExactly)
{
    GBMGenerator generator;
    auto config = generator.default_config();
    config.n_steps = 25;
    const auto path = generator.generate(0x1234, config);

    SyntheticTransport transport(path.bars);
    ASSERT_TRUE(transport.open());

    CsvBarParser parser;
    const auto header = transport.read_line();
    ASSERT_TRUE(header.has_value());
    ASSERT_TRUE(parser.parse_header(*header));

    for (const auto& expected : path.bars)
    {
        const auto line = transport.read_line();
        ASSERT_TRUE(line.has_value());
        const auto actual = parser.parse_record(*line);
        ASSERT_TRUE(actual.has_value()) << *line;
        EXPECT_EQ(actual->date, expected.date);
        EXPECT_EQ(actual->symbol, expected.symbol);
        EXPECT_DOUBLE_EQ(actual->open, expected.open);
        EXPECT_DOUBLE_EQ(actual->high, expected.high);
        EXPECT_DOUBLE_EQ(actual->low, expected.low);
        EXPECT_DOUBLE_EQ(actual->close, expected.close);
        EXPECT_EQ(actual->volume, expected.volume);
    }

    EXPECT_FALSE(transport.read_line().has_value());
}

TEST(GBMGenerator, SyntheticDepthUsesPositiveIntegralQuantities) {
    GBMGenerator gen;
    McGeneratorConfig cfg = gen.default_config();
    cfg.n_steps = 10;
    cfg.emit_synthetic_l2 = true;
    cfg.depth_noise = 5.0;

    const auto path = gen.generate(7, cfg);

    ASSERT_EQ(path.l2_snapshots.size(), 10u);
    for (const auto& snapshot : path.l2_snapshots) {
        ASSERT_EQ(snapshot.bids.size(), 3u);
        ASSERT_EQ(snapshot.asks.size(), 3u);
        for (const auto& level : snapshot.bids)
            EXPECT_GE(level.quantity, 1);
        for (const auto& level : snapshot.asks)
            EXPECT_GE(level.quantity, 1);
    }
}
