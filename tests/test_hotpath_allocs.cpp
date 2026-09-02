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
#include "execution/fee_model.h"
#include "strategy/sma/sma_strategy.h"

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

class physical_flip_strategy final : public IStrategy
{
    int bar_count_ = 0;
public:
    std::size_t fill_callbacks = 0;

    std::optional<order_event> on_market(const market_event& m) override
    {
        if (bar_count_++ == 0)
        {
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::buy, 10.0,
                               m.get_close());
        }
        if (bar_count_ == 2)
        {
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::sell, 15.0,
                               m.get_close());
        }
        return std::nullopt;
    }

    void on_fill(const fill_event&, std::uint64_t) override { ++fill_callbacks; }
};

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

    // Resting-fill mechanism + dashboard/lifetime work + strategy exit_intent
    // vectors (SL/TP on every entry) raise one-time setup cost; 1000-bar
    // steady-state stays flat (see SmaSynthetic_1000Bars).
    EXPECT_LE(delta.count, 4000u);
    EXPECT_LE(delta.bytes, 30000000u);
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

    // Baseline Phase 5/6 (dashboard extraction + State ownership safety): ~33M bytes observed
    // (snapshot builder work during publish + safety overhead)
    //
    // F-02 (docs/todos/11-F-forensic-lifecycle-audit.md) raised the allocation
    // count from ~62k to ~106k, and the increase is the fix working. This
    // fixture breaches the drawdown limit early; with soft portfolio limits in
    // backtest mode every later order is rejected. Before F-02 the rejection
    // was never reported to the strategy, so its optimistic position gate
    // stayed set and it silently stopped emitting orders for the rest of the
    // run — the budget below was calibrated against a deadlocked strategy that
    // placed ~76 orders in 1000 bars. The strategy now correctly returns to
    // flat after each rejection and keeps signalling (~1000 orders), and each
    // order costs the same as it always did. Per-event steady state is
    // unchanged: the idle scenarios (A/C/D) and the pool-growth assertion
    // below are untouched.
    EXPECT_LE(delta.count, 115000u);
    EXPECT_LE(delta.bytes, 35000000u);

    EXPECT_EQ(read_pool_grows(eng).total(), 0u);
}

TEST(HotpathAllocs, PhysicalFlipWithTieredFees_OnePooledEventPerExecution)
{
    silence_cout quiet;

    auto dh = make_bars(6);
    auto ob = std::make_shared<orderbook>();
    seed_book(ob, 100.0);
    auto strat = std::make_shared<physical_flip_strategy>();

    engine_config cfg = base_cfg(thread_preset::inline_mode);
    cfg.execution_bar_delay = 1;
    // Exercise the default CLI fee model on the complete physical-fill path;
    // the virtual fee lookup must not grow a pool after warmup.
    cfg.fee_model = std::make_shared<TieredFeeModel>(0.001, 0.001);
    engine eng(dh, ob, strat, cfg);

    measure_window window;
    eng.run();
    const snapshot delta = window.total();
    maybe_print_baseline("PhysicalFlip", delta);

    // The bounds characterize the complete warmed engine path, including its
    // current dashboard publication cost. The semantic assertions ensure a
    // regression cannot restore a synthetic second flip event unnoticed.
    EXPECT_LE(delta.count, 4000u);
    EXPECT_LE(delta.bytes, 30000000u);
    EXPECT_EQ(read_pool_grows(eng).total(), 0u);
    EXPECT_EQ(strat->fill_callbacks, 2u);
    const auto report = eng.get_analytics().generate_report();
    EXPECT_EQ(report.total_fills, 2u);
    EXPECT_GT(report.total_commission, 0.0);
    ASSERT_EQ(report.trades.size(), 2u);
    EXPECT_DOUBLE_EQ(report.trades.back().quantity, 15.0);
}
