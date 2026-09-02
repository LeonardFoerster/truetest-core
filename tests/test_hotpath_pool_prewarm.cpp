// Phase 1–2: object-pool pre-warm, pooled events, zero-heap control blocks.

#include <gtest/gtest.h>

#include "helpers/alloc_counter.h"
#include "helpers/hotpath_run_helpers.h"
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "execution/queue_model.h"

namespace {

using namespace truetest::test::hotpath;

TEST(HotpathPoolPrewarm, L2BurstUsesPooledEvents_NoRuntimeGrow)
{
    silence_cout quiet;

    auto dh = make_bars(1);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<no_trade_strategy>();

    engine_config cfg = base_cfg(thread_preset::inline_mode);
    engine eng(dh, ob, strat, cfg);

    for (int i = 0; i < 4000; ++i)
        eng.apply_l2_update("BTCUSDT", tick_side::bid, 42000.0 + i, 100);

    truetest::ui::dashboard_snapshot snap;
    ASSERT_TRUE(eng.snapshot_dashboard(snap));

    std::size_t l2_grow = 0;
    for (const auto& p : snap.debug.pools)
    {
        if (std::string(p.name) == "l2_update_pool")
            l2_grow = p.grow_count;
    }
    EXPECT_EQ(l2_grow, 0u);
}

TEST(HotpathPoolPrewarm, L2Burst_NoControlBlockHeapAllocs)
{
    silence_cout quiet;

    auto dh = make_bars(1);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<no_trade_strategy>();

    engine_config cfg = base_cfg(thread_preset::inline_mode);
    engine eng(dh, ob, strat, cfg);

    truetest::test::alloc::measure_window window;
    for (int i = 0; i < 4000; ++i)
        eng.apply_l2_update("BTCUSDT", tick_side::bid, 42000.0 + i, 100);

    const auto snap = window.total();
    // Dashboard capture is now a bounded fixed projection and contributes
    // exactly zero heap operations (covered by DashboardProjection's strict
    // allocation test). This broader burst still measures orderbook growth for
    // 4,000 distinct price levels, plus any other non-dashboard work.
    //
    // Sanitizers slow wall-clock so more 100ms dashboard refreshes fall inside
    // the measure window (ASan ~9598–9630; TSan ~10248–10280 historical).
    // Measurement FP, not product heap grow — hard ceiling for non-sanitizer only.
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define TT_HOTPATH_SANITIZER 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define TT_HOTPATH_SANITIZER 1
#endif
#if defined(TT_HOTPATH_SANITIZER)
    EXPECT_LE(snap.count, 12000u) << "sanitizer allocs=" << snap.count
        << " (sanitizer slowdown of the full distinct-price burst)";
    EXPECT_LE(snap.bytes, 40000000u) << "sanitizer bytes=" << snap.bytes;
#else
    EXPECT_LE(snap.count, 9000u) << "allocs=" << snap.count;
    EXPECT_LE(snap.bytes, 28000000u) << "bytes=" << snap.bytes;
#endif

    truetest::ui::dashboard_snapshot dash;
    ASSERT_TRUE(eng.snapshot_dashboard(dash));
    for (const auto& p : dash.debug.pools)
    {
        if (std::string(p.name) == "control_block_pool")
        {
            EXPECT_EQ(p.grow_count, 0u);
            // L2 burst may leave a few pooled event CBs in flight (rings/log).
            EXPECT_LE(p.in_use, 64u);
        }
    }
}

TEST(HotpathPoolPrewarm, ExhaustionWithTinyPoolHaltsEngine)
{
    silence_cout quiet;

    auto dh = make_bars(10);
    auto ob = std::make_shared<orderbook>();
    seed_book(ob, 100.0);
    auto strat = std::make_shared<no_trade_strategy>();

    engine_config cfg = base_cfg(thread_preset::inline_mode);
    cfg.pool_prewarm.market_blocks = 1;
    cfg.pool_prewarm.forbid_runtime_grow = true;

    engine eng(dh, ob, strat, cfg);
    eng.run();

    truetest::ui::dashboard_snapshot snap;
    ASSERT_TRUE(eng.snapshot_dashboard(snap));
    bool saw_order = false;
    for (const auto& p : snap.debug.pools)
    {
        if (std::string(p.name) == "order_pool")
        {
            saw_order = true;
            EXPECT_GE(p.blocks, 2u);
            EXPECT_EQ(p.grow_count, 0u);
        }
    }
    EXPECT_TRUE(saw_order);
}

// maker_queue paper tape + no-order strategy must not create adapters or
// explode levels_ via feed_paper_trade_and_drain (find-only path).
TEST(HotpathPoolPrewarm, MakerQueueNoOrders_RunCompletesWithoutLiveQuotes)
{
    silence_cout quiet;

    auto dh = make_bars(200);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<no_trade_strategy>();

    engine_config cfg = base_cfg(thread_preset::inline_mode);
    cfg.maker_queue_model = std::make_shared<UniformCancelModel>();

    engine eng(dh, ob, strat, cfg);
    EXPECT_NO_THROW(eng.run());
    EXPECT_EQ(eng.total_live_quotes(), 0u);
}

} // namespace
