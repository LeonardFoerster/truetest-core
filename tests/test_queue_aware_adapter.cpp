#include <gtest/gtest.h>
#include "execution/queue_aware_book_adapter.h"
#include "execution/queue_model.h"
#include "execution/latency_model.h"

#include <chrono>
#include <memory>

using namespace std::chrono_literals;

namespace {

auto t0()       { return std::chrono::system_clock::time_point(std::chrono::milliseconds(0)); }
auto t_at(int ms) { return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms)); }

order_event make_limit(uint64_t oid, const std::string& sym,
                       order_side side, double price, double qty,
                       std::chrono::system_clock::time_point ts = std::chrono::system_clock::time_point())
{
    order_event o(ts, sym, order_type::limit, side, qty, price);
    o.set_order_id(oid);
    o.set_earliest_eligible_ts(ts);
    return o;
}

}

// ---- Seeding -----------------------------------------------------------

TEST(QueueAwareBookAdapter, FreshLevel_SubmitDoesNotJoinFront)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    // No L2: default conservative — treat as back of queue (not front).
    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 5.0));
    EXPECT_EQ(a.live_order_count(), 1u);
    EXPECT_EQ(a.avg_queue_position_bps(), 10000u);

    // Tape cannot fill without L2 (FR-queue-no-l2-optimistic-join).
    a.on_trade("X", 100.0, 1000.0, t_at(100));
    std::vector<fill_event> fills;
    EXPECT_FALSE(a.poll_fills(fills));
    EXPECT_EQ(a.live_order_count(), 1u);
}

TEST(QueueAwareBookAdapter, OptInJoinFrontWithoutL2)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);
    a.set_join_front_without_l2(true);

    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 5.0));
    EXPECT_EQ(a.avg_queue_position_bps(), 0u);
    a.on_trade("X", 100.0, 5.0, t_at(100));
    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    EXPECT_NEAR(fills[0].get_filled_quantity(), 5.0, 1e-9);
}

TEST(QueueAwareBookAdapter, SubmitAfterL2_JoinsBackOfQueue)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    // Venue reports 20 units resting at our bid level.
    a.on_l2_update("X", order_side::buy, 100.0, 20.0);
    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 5.0));

    // Expected: size_ahead = 20, aggregate = 20 -> we're at 100% of the
    // queue (fully at the back).
    EXPECT_EQ(a.avg_queue_position_bps(), 10000u);
}

// ---- Trade consumption -------------------------------------------------

TEST(QueueAwareBookAdapter, TradeConsumesAheadFirst_NoFill)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    a.on_l2_update("X", order_side::buy, 100.0, 20.0);
    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 5.0));

    // Trade of 10 at 100 -> consumes 10 from the front; size_ahead: 20 -> 10.
    // We shouldn't fill yet.
    a.on_trade("X", 100.0, 10.0, t_at(100));

    std::vector<fill_event> fills;
    EXPECT_FALSE(a.poll_fills(fills));
    EXPECT_EQ(fills.size(), 0u);
    EXPECT_EQ(a.live_order_count(), 1u);
}

TEST(QueueAwareBookAdapter, TradeExceedsAhead_FillsTheExcess)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    a.on_l2_update("X", order_side::buy, 100.0, 20.0);
    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 5.0));

    // Trade of 25 at 100 -> eats the 20 ahead, reaches us with 5 left ->
    // we fill 5 at 100.
    a.on_trade("X", 100.0, 25.0, t_at(100));

    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 1u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 5.0, 1e-9);
    EXPECT_NEAR(fills[0].get_fill_price(), 100.0, 1e-9);
    EXPECT_EQ(a.live_order_count(), 0u);
}

TEST(QueueAwareBookAdapter, PartialFill_OrderStaysLive)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    a.on_l2_update("X", order_side::buy, 100.0, 20.0);
    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 10.0));

    // Trade of 23 at 100 -> 20 ahead consumed, 3 reaches us; we have 10
    // to fill so we take 3 and keep 7 qty_remaining.
    a.on_trade("X", 100.0, 23.0, t_at(100));

    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 3.0, 1e-9);
    // FR-QA-partial-remaining: remaining_qty must be set so is_partial() is true.
    EXPECT_NEAR(fills[0].get_remaining_qty(), 7.0, 1e-9);
    EXPECT_TRUE(fills[0].is_partial());
    EXPECT_EQ(a.live_order_count(), 1u);

    // Second print drains residual — full fill, not partial.
    a.on_trade("X", 100.0, 7.0, t_at(101));
    fills.clear();
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 7.0, 1e-9);
    EXPECT_NEAR(fills[0].get_remaining_qty(), 0.0, 1e-9);
    EXPECT_FALSE(fills[0].is_partial());
    EXPECT_EQ(a.live_order_count(), 0u);
}

TEST(QueueAwareBookAdapter, SweepRestingRange_BuyLimitInBarLow)
{
    // Bar path: buy limit between open/close that low touches must fill
    // even though close-only tape would miss exact price match.
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    a.submit_order(make_limit(1, "X", order_side::buy, 99.5, 5.0, t_at(10)));
    // Bar: open/close 100, low 99, high 100.5 — limit at 99.5 is traversed.
    EXPECT_TRUE(a.sweep_resting_range("X", 99.0, 100.5, t_at(100)));

    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 1u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 5.0, 1e-9);
    EXPECT_NEAR(fills[0].get_fill_price(), 99.5, 1e-9);  // maker at limit
    EXPECT_NEAR(fills[0].get_remaining_qty(), 0.0, 1e-9);
    EXPECT_EQ(a.live_order_count(), 0u);
}

TEST(QueueAwareBookAdapter, SweepRestingRange_MissesWhenNotTraversed)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    // Buy far below bar low — not traversed.
    a.submit_order(make_limit(1, "X", order_side::buy, 90.0, 5.0, t_at(10)));
    EXPECT_FALSE(a.sweep_resting_range("X", 99.0, 100.5, t_at(100)));
    EXPECT_EQ(a.live_order_count(), 1u);
}

TEST(QueueAwareBookAdapter, TradeAtDifferentPrice_Ignored)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    a.on_l2_update("X", order_side::buy, 100.0, 20.0);
    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 5.0));

    // Trade at a different price doesn't advance our queue.
    a.on_trade("X", 99.5, 100.0, t_at(100));

    std::vector<fill_event> fills;
    EXPECT_FALSE(a.poll_fills(fills));
    EXPECT_EQ(a.live_order_count(), 1u);
}

TEST(QueueAwareBookAdapter, SamePriceMultiOrder_ResidualIsDeterministic)
{
    // Multiple resting limits at one price share trade residual qty.
    // Fill order must be FIFO by submit_ts then order_id — not hash order.
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    // Seed L2 so orders join with size_ahead=0 (empty level = front after L2).
    a.on_l2_update("X", order_side::buy, 100.0, 0.0);

    // Higher order_id submitted earlier so order_id-only sort would diverge.
    a.submit_order(make_limit(2, "X", order_side::buy, 100.0, 5.0, t_at(10)));
    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 5.0, t_at(20)));

    // Both at front (size_ahead=0). Trade of 7 fills oid 2 fully (5) then oid 1 for 2.
    a.on_trade("X", 100.0, 7.0, t_at(100));

    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].get_order_id(), 2u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 5.0, 1e-9);
    EXPECT_EQ(fills[1].get_order_id(), 1u);
    EXPECT_NEAR(fills[1].get_filled_quantity(), 2.0, 1e-9);
    EXPECT_EQ(a.live_order_count(), 1u);
}

// ---- Cancel attribution via IQueueModel --------------------------------

TEST(QueueAwareBookAdapter, FrontCancel_AdvancesQueueOnShrinkage)
{
    auto qm = std::make_shared<FrontCancelModel>();
    QueueAwareBookAdapter a(qm);

    a.on_l2_update("X", order_side::buy, 100.0, 20.0);
    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 5.0));

    // Level shrinks from 20 -> 12 with NO trades observed -> 8 cancels.
    // FrontCancelModel: size_ahead = 20 - 8 = 12.
    a.on_l2_update("X", order_side::buy, 100.0, 12.0);

    // A trade of 13 at 100 arrives: consumes 12 ahead, 1 reaches us.
    a.on_trade("X", 100.0, 13.0, t_at(100));

    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    EXPECT_NEAR(fills[0].get_filled_quantity(), 1.0, 1e-9);
}

TEST(QueueAwareBookAdapter, BackCancel_NeverAdvances)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    a.on_l2_update("X", order_side::buy, 100.0, 20.0);
    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 5.0));

    // Level shrinks from 20 -> 12 with NO trades observed -> 8 cancels.
    // BackCancelModel: size_ahead remains 20.
    a.on_l2_update("X", order_side::buy, 100.0, 12.0);

    // Trade of 13 at 100: consumes 13 of our 20-unit ahead-depth; no fill.
    a.on_trade("X", 100.0, 13.0, t_at(100));

    std::vector<fill_event> fills;
    EXPECT_FALSE(a.poll_fills(fills));
}

TEST(QueueAwareBookAdapter, L2Growth_DoesNotChangeQueuePosition)
{
    auto qm = std::make_shared<FrontCancelModel>();
    QueueAwareBookAdapter a(qm);

    a.on_l2_update("X", order_side::buy, 100.0, 20.0);
    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 5.0));

    // Level grows 20 -> 30 (someone joined the back). Our queue position
    // is unchanged (size_ahead still 20).
    a.on_l2_update("X", order_side::buy, 100.0, 30.0);

    a.on_trade("X", 100.0, 20.0, t_at(100));  // consume exactly what's ahead
    std::vector<fill_event> fills_empty;
    EXPECT_FALSE(a.poll_fills(fills_empty));

    a.on_trade("X", 100.0, 1.0, t_at(101));   // one more unit reaches us
    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    EXPECT_NEAR(fills[0].get_filled_quantity(), 1.0, 1e-9);
}

TEST(QueueAwareBookAdapter, TradesAttributedBeforeCancelInference)
{
    auto qm = std::make_shared<FrontCancelModel>();
    QueueAwareBookAdapter a(qm);

    a.on_l2_update("X", order_side::buy, 100.0, 20.0);
    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 5.0));

    // A trade of 5 at 100 advances our queue to 15. Then L2 shows the
    // aggregate is now 12 - i.e. 20 -> 12 = 8 reduction, of which 5 was
    // the trade. Only 3 units are cancels. FrontCancel: 15 - 3 = 12.
    a.on_trade("X", 100.0, 5.0, t_at(100));
    a.on_l2_update("X", order_side::buy, 100.0, 12.0);

    // A trade of 13 at 100 would consume 12 ahead, leaving 1 for us.
    a.on_trade("X", 100.0, 13.0, t_at(101));
    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    EXPECT_NEAR(fills[0].get_filled_quantity(), 1.0, 1e-9);
}

// ---- Cancel (with and without latency) ---------------------------------

TEST(QueueAwareBookAdapter, CancelWithoutLatency_RemovesImmediately)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 5.0));
    EXPECT_TRUE(a.cancel_order(1));
    EXPECT_EQ(a.live_order_count(), 0u);
}

TEST(QueueAwareBookAdapter, CancelWithLatency_DefersUntilAdvanceTime)
{
    auto qm  = std::make_shared<BackCancelModel>();
    auto lat = std::make_shared<FixedLatencyModel>(
        latency_duration(0),
        latency_duration(0),
        std::chrono::duration_cast<latency_duration>(100ms));
    QueueAwareBookAdapter a(qm, nullptr, lat);

    a.on_l2_update("X", order_side::buy, 100.0, 20.0);
    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 5.0));
    a.advance_time(t0());

    // T=50ms: cancel (eligible at 150ms).
    a.advance_time(t_at(50));
    EXPECT_TRUE(a.cancel_order(1));
    EXPECT_EQ(a.live_order_count(), 1u);

    // T=80ms: within window -> trade crossing us still fills.
    a.advance_time(t_at(80));
    a.on_trade("X", 100.0, 25.0, t_at(80));
    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    EXPECT_EQ(fills[0].get_order_id(), 1u);

    // Order removed by fill; pending cancel is now stale but harmless.
    EXPECT_EQ(a.live_order_count(), 0u);
}

// ---- Snapshot --------------------------------------------------------

TEST(QueueAwareBookAdapter, SnapshotSeedsBothSides)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    std::vector<std::pair<double,double>> bids = {{99.0, 10.0}, {100.0, 5.0}};
    std::vector<std::pair<double,double>> asks = {{101.0, 7.0}, {102.0, 3.0}};
    a.on_l2_snapshot("X", bids, asks);

    a.submit_order(make_limit(1, "X", order_side::buy,  100.0, 2.0));
    a.submit_order(make_limit(2, "X", order_side::sell, 101.0, 2.0));

    // Buy  at 100: size_ahead = 5 (from bids).
    // Sell at 101: size_ahead = 7 (from asks).
    // Both orders fully at the back -> avg 100% (10000 bps).
    EXPECT_EQ(a.avg_queue_position_bps(), 10000u);
}

// ---- Diagnostics ------------------------------------------------------

TEST(QueueAwareBookAdapter, AvgQueuePositionReflectsOrderMix)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    a.on_l2_update("X", order_side::buy, 100.0, 20.0);
    // Order 1: submit when aggregate = 20 -> size_ahead = 20.
    a.submit_order(make_limit(1, "X", order_side::buy, 100.0, 1.0));

    // Trade of 10 advances order 1's queue (size_ahead: 20 -> 10) without
    // touching aggregate_size - aggregate reflects the last L2 snapshot,
    // and the next L2 update will bring the venue's new truth.
    a.on_trade("X", 100.0, 10.0, t_at(100));

    // Order 2 submitted before any new L2 update -> uses stale aggregate
    // (still 20), so size_ahead = 20.
    a.submit_order(make_limit(2, "X", order_side::buy, 100.0, 1.0));

    // Order 1: 10 / 20 = 50% = 5000 bps.
    // Order 2: 20 / 20 = 100% = 10000 bps.
    // Avg = 7500 bps.
    EXPECT_EQ(a.avg_queue_position_bps(), 7500u);
    EXPECT_EQ(a.live_order_count(), 2u);
}

// ---- Hot-path / determinism regressions --------------------------------

TEST(QueueAwareBookAdapter, OnTradeNoOrders_DoesNotGrowLevels)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    // Paper tape under maker_queue feeds every bar; empty adapter must not
    // insert a levels_ node per unique price (R1 unbounded growth).
    for (int i = 0; i < 500; ++i)
        a.on_trade("X", 100.0 + i * 0.01, 1.0, t_at(i));

    EXPECT_EQ(a.tracked_level_count(), 0u);
    EXPECT_EQ(a.live_order_count(), 0u);
}

TEST(QueueAwareBookAdapter, SamePriceMultiOrder_FillsInSubmitFifoOrder)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);

    // Empty L2 level → join front (size_ahead=0). Residual trade qty shared.
    a.on_l2_update("X", order_side::buy, 100.0, 0.0);
    a.submit_order(make_limit(10, "X", order_side::buy, 100.0, 3.0, t_at(10)));
    a.submit_order(make_limit(20, "X", order_side::buy, 100.0, 3.0, t_at(20)));

    // Trade of 4: FIFO by submit_ts → oid 10 fills 3, oid 20 fills 1.
    a.on_trade("X", 100.0, 4.0, t_at(100));
    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].get_order_id(), 10u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 3.0, 1e-9);
    EXPECT_EQ(fills[1].get_order_id(), 20u);
    EXPECT_NEAR(fills[1].get_filled_quantity(), 1.0, 1e-9);
    EXPECT_EQ(a.live_order_count(), 1u);
}

TEST(QueueAwareBookAdapter, SweepRestingRange_VolumeCapsPartial)
{
    auto qm = std::make_shared<BackCancelModel>();
    QueueAwareBookAdapter a(qm);
    a.submit_order(make_limit(1, "X", order_side::buy, 99.5, 10.0, t_at(10)));
    // Bar volume 3 → partial fill, residual stays live.
    EXPECT_TRUE(a.sweep_resting_range("X", 99.0, 100.5, t_at(100), /*bar_volume=*/3.0));
    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 3.0, 1e-9);
    EXPECT_NEAR(fills[0].get_remaining_qty(), 7.0, 1e-9);
    EXPECT_EQ(a.live_order_count(), 1u);
}
