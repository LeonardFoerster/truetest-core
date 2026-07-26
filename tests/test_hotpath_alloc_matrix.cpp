// Phase 0: six reference scenarios for hot-path allocation + pool growth baselines.
// See summary migration plan Phase 0 — matrix documents current behaviour before
// Phase 1+ drives alloc counts toward zero.
//
// Re-baseline:
//   TRUETEST_REBASELINE_ALLOCS=1 ./build/truetest_tests --gtest_filter='HotpathAllocMatrix.*'

#include <gtest/gtest.h>

#include "helpers/alloc_counter.h"
#include "helpers/hotpath_run_helpers.h"

#include "engine/engine.h"
#include "execution/latency_model.h"
#include "strategy/sma_strategy.h"

#include <cstdlib>
#include <iostream>

namespace {

using truetest::test::alloc::measure_window;
using truetest::test::alloc::snapshot;
using namespace truetest::test::hotpath;

struct scenario_limits
{
    const char* name;
    std::uint64_t max_allocs;
    std::uint64_t max_bytes;
};

class no_trade_ticks : public no_trade_strategy
{
public:
    std::optional<order_event> on_tick(const tick_event&) override
    {
        return std::nullopt;
    }
};

void maybe_print(const scenario_limits& lim, const snapshot& s)
{
    if (std::getenv("TRUETEST_REBASELINE_ALLOCS"))
    {
        std::cerr << "[HotpathAllocMatrix] " << lim.name
                  << " count=" << s.count << " bytes=" << s.bytes << '\n';
    }
}

void run_scenario(const scenario_limits& lim,
                  const std::shared_ptr<data_handler>& dh,
                  const std::shared_ptr<IStrategy>& strat,
                  engine_config cfg,
                  bool tick_mode = false)
{
    silence_cout quiet;
    auto ob = std::make_shared<orderbook>();
    seed_book(ob, 100.0);

    engine eng(dh, ob, strat, cfg);

    measure_window window;
    if (tick_mode)
        eng.run_tick_data();
    else
        eng.run();
    const snapshot delta = window.total();
    maybe_print(lim, delta);

    EXPECT_LE(delta.count, lim.max_allocs) << lim.name;
    EXPECT_LE(delta.bytes, lim.max_bytes) << lim.name;
    EXPECT_EQ(read_pool_grows(eng).total(), 0u) << lim.name;
}

} // namespace

// A — Bar backtest, no trades (market_event only).
TEST(HotpathAllocMatrix, A_BarIdle_1000)
{
    // Baseline Phase 4: count≈59591 bytes≈10.7M
    // Phase 5/6 post dashboard_snapshot_builder extraction + object_pool lifetime token:
    // snapshot builder (called from publish) + memory /proc parsing + vector materialization
    // add measurable bytes under the alloc window in these fast tests.
    run_scenario({"A_BarIdle_1000", 62000, 35000000},
                 make_bars(1000),
                 std::make_shared<no_trade_strategy>(),
                 base_cfg(thread_preset::inline_mode));
}

// B — Bar backtest, SMA trades.
TEST(HotpathAllocMatrix, B_BarSma_1000)
{
    // Baseline Phase 5 (post object_pool lifetime token + Returner): count~59k bytes~11.03M
    // (slightly higher bytes due to larger per-acquire shared_ptr control block for safety token)
    run_scenario({"B_BarSma_1000", 62000, 35000000},
                 make_bars(1000),
                 std::make_shared<sma_strategy>(5),
                 base_cfg(thread_preset::inline_mode));
}

// C — Tick backtest, 3600 ticks (~10/s for 6 min scaled).
TEST(HotpathAllocMatrix, C_TickIdle_3600)
{
    engine_config cfg = base_cfg(thread_preset::inline_mode);
    // Baseline Phase 5 (post object_pool lifetime token + Returner): ~21.70M
    run_scenario({"C_TickIdle_3600", 175000, 46000000},
                 make_ticks(3600),
                 std::make_shared<no_trade_ticks>(),
                 cfg,
                 true);
}

// D — Threaded bar backtest (standard = logging + risk_stats rings).
TEST(HotpathAllocMatrix, D_BarIdle_1000_StandardPreset)
{
    // Baseline Phase 5 (post object_pool lifetime token + Returner): ~11.54M
    run_scenario({"D_BarIdle_1000_Standard", 62000, 35000000},
                 make_bars(1000),
                 std::make_shared<no_trade_strategy>(),
                 base_cfg(thread_preset::standard));
}

// E — Latency-delayed orders (pending_orders_ holds shared_ptrs across bars).
TEST(HotpathAllocMatrix, E_BarLatencyPending_500)
{
    engine_config cfg = base_cfg(thread_preset::inline_mode);
    cfg.latency_model = std::make_shared<FixedLatencyModel>(
        latency_duration(500'000));

    // Baseline Phase 4: count≈31404 bytes≈8.91M
    run_scenario({"E_BarLatencyPending_500", 33000, 33000000},
                 make_bars(500),
                 std::make_shared<sma_strategy>(5),
                 cfg);
}

// F — L2 update burst (depth20-style 40 levels × 100 messages).
TEST(HotpathAllocMatrix, F_L2UpdateBurst_4000)
{
    silence_cout quiet;

    auto dh = make_bars(1);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<no_trade_strategy>();

    engine_config cfg = base_cfg(thread_preset::inline_mode);
    engine eng(dh, ob, strat, cfg);

    measure_window window;
    for (int msg = 0; msg < 100; ++msg)
    {
        for (int lvl = 0; lvl < 20; ++lvl)
        {
            eng.apply_l2_update("BTCUSDT", tick_side::bid,
                                42000.0 - lvl, 1'000'000);
            eng.apply_l2_update("BTCUSDT", tick_side::ask,
                                42001.0 + lvl, 1'000'000);
        }
    }

    const snapshot delta = window.total();
    // Baseline Phase 4: count≈8152 bytes≈3.93M (L2 orders pooled; adapter vecs).
    const scenario_limits lim{"F_L2UpdateBurst_4000", 9000, 27000000};
    maybe_print(lim, delta);

    EXPECT_LE(delta.count, lim.max_allocs);
    EXPECT_LE(delta.bytes, lim.max_bytes);
    EXPECT_EQ(read_pool_grows(eng).total(), 0u);
}