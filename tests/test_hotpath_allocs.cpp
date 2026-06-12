// Phase 0: hot-path heap allocation baseline for the SMA golden configuration.
// Measures post-warmup allocations during engine::run() and records upper bounds
// so later migration phases can drive these toward zero.
//
// Re-baseline (after intentional alloc changes):
//   TRUETEST_REBASELINE_ALLOCS=1 ./build/truetest_tests --gtest_filter='HotpathAllocs.*'

#include <gtest/gtest.h>

#include "helpers/alloc_counter.h"
#include "helpers/hotpath_run_helpers.h"

#include "engine/engine.h"
#include "data/csv_data_source.h"
#include "data/data_handler.h"
#include "strategy/sma_strategy.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

using truetest::test::alloc::measure_window;
using truetest::test::alloc::snapshot;
using namespace truetest::test::hotpath;

std::shared_ptr<data_handler> load_golden_csv()
{
    const auto path = std::filesystem::path(TEST_FIXTURES_DIR).parent_path()
                    / "golden" / "sma_basic.csv";
    auto dh = std::make_shared<data_handler>();
    CsvDataSource src(path.string());
    (void)src.load_data(dh);
    return dh;
}

void maybe_print_baseline(const char* name, const snapshot& s)
{
    if (std::getenv("TRUETEST_REBASELINE_ALLOCS"))
    {
        std::cerr << "[HotpathAllocs baseline] " << name
                  << " count=" << s.count << " bytes=" << s.bytes << '\n';
    }
}

} // namespace

TEST(HotpathAllocs, SmaGolden_30Bars_PostWarmupUpperBound)
{
    silence_cout quiet;

    auto dh = load_golden_csv();
    auto ob = std::make_shared<orderbook>();
    seed_book(ob, 100.0);
    auto strat = std::make_shared<sma_strategy>(5);

    engine_config cfg = base_cfg(thread_preset::inline_mode);
    engine eng(dh, ob, strat, cfg);

    measure_window window;
    eng.run();
    const snapshot delta = window.total();
    maybe_print_baseline("SmaGolden_30Bars", delta);

    // Baseline Phase 4: count≈2026 bytes≈6.08M (orderbook bodies pooled; CB heap).
    // Re-baselined ≈2643 with the resting-fill mechanism: MM quote-pull
    // bookkeeping (one-time) plus this scenario now producing 16 fills
    // instead of 3 (each fill walks the full event pipeline). The
    // 1000-bar test confirms steady-state per-bar cost stays flat.
    EXPECT_LE(delta.count, 3000u);
    EXPECT_LE(delta.bytes, 6500000u);
    EXPECT_EQ(read_pool_grows(eng).total(), 0u);
}

TEST(HotpathAllocs, SmaSynthetic_1000Bars_PostWarmupUpperBound)
{
    silence_cout quiet;

    auto dh = make_bars(1000);
    auto ob = std::make_shared<orderbook>();
    seed_book(ob, 100.0);
    auto strat = std::make_shared<sma_strategy>(5);

    engine_config cfg = base_cfg(thread_preset::inline_mode);
    engine eng(dh, ob, strat, cfg);

    measure_window window;
    eng.run();
    const snapshot delta = window.total();
    maybe_print_baseline("SmaSynthetic_1000Bars", delta);

    // Baseline Phase 4: count≈59756 bytes≈10.7M (orderbook bodies pooled).
    EXPECT_LE(delta.count, 62000u);
    EXPECT_LE(delta.bytes, 11000000u);
    EXPECT_EQ(read_pool_grows(eng).total(), 0u);
}