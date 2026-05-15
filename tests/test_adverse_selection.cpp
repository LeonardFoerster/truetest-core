#include <gtest/gtest.h>
#include "analytics/adverse_selection_tracker.h"

#include <chrono>
#include <cmath>

namespace {

using namespace std::chrono_literals;

auto epoch_ms(int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

// Constructor args: (timestamp, symbol, order_id, side, filled_qty, price, commission).
fill_event make_fill(int64_t ts_ms, const std::string& sym,
                     order_side side, double price, double qty = 1.0)
{
    return fill_event(epoch_ms(ts_ms), sym, /*order_id=*/1, side, qty, price, /*comm=*/0.0);
}

}

TEST(AdverseSelection, EmptyState)
{
    AdverseSelectionTracker t;
    EXPECT_EQ(t.sample_count(), 0u);
    EXPECT_EQ(t.pending_count(), 0u);
    EXPECT_DOUBLE_EQ(t.mean_bps(),  0.0);
    EXPECT_DOUBLE_EQ(t.stdev_bps(), 0.0);
}

TEST(AdverseSelection, HorizonNotElapsed_FillStaysPending)
{
    AdverseSelectionTracker t({10s, 1024});
    t.on_fill(make_fill(1000, "X", order_side::buy, 100.0));
    // Mark at t=5s, horizon ends at t=11s → not yet elapsed.
    t.on_mark("X", 110.0, epoch_ms(5000));
    EXPECT_EQ(t.sample_count(), 0u);
    EXPECT_EQ(t.pending_count(), 1u);
}

TEST(AdverseSelection, BuyFill_MarketMovesUp_PositiveMarkout)
{
    AdverseSelectionTracker t({10s, 1024});
    t.on_fill(make_fill(1000, "X", order_side::buy, 100.0));
    // Horizon ends at t=11s; mark at t=15s with price 101 → +100 bps.
    t.on_mark("X", 101.0, epoch_ms(15000));
    EXPECT_EQ(t.sample_count(), 1u);
    EXPECT_EQ(t.pending_count(), 0u);
    EXPECT_NEAR(t.mean_bps(), 100.0, 1e-6);
}

TEST(AdverseSelection, BuyFill_MarketMovesDown_NegativeMarkout)
{
    AdverseSelectionTracker t({10s, 1024});
    t.on_fill(make_fill(1000, "X", order_side::buy, 100.0));
    t.on_mark("X", 99.0, epoch_ms(15000));   // -100 bps
    EXPECT_NEAR(t.mean_bps(), -100.0, 1e-6);
}

TEST(AdverseSelection, SellFill_SignFlipsCorrectly)
{
    AdverseSelectionTracker t({10s, 1024});
    // Sold at 100; market dropped to 99 — good for the seller, positive mark.
    t.on_fill(make_fill(1000, "X", order_side::sell, 100.0));
    t.on_mark("X", 99.0, epoch_ms(15000));
    EXPECT_NEAR(t.mean_bps(), +100.0, 1e-6);
}

TEST(AdverseSelection, MultipleFills_WelfordAverage)
{
    AdverseSelectionTracker t({10s, 1024});
    t.on_fill(make_fill(1000, "X", order_side::buy, 100.0));
    t.on_fill(make_fill(2000, "X", order_side::buy, 100.0));
    t.on_fill(make_fill(3000, "X", order_side::buy, 100.0));
    // Mark at t=20s with price 102 (200 bps for each fill).
    t.on_mark("X", 102.0, epoch_ms(20'000));
    EXPECT_EQ(t.sample_count(), 3u);
    EXPECT_NEAR(t.mean_bps(), 200.0, 1e-6);
    EXPECT_NEAR(t.stdev_bps(), 0.0, 1e-6);   // all three identical → sd = 0
}

TEST(AdverseSelection, MixedSymbols_OnlyMatchingOneDrained)
{
    AdverseSelectionTracker t({10s, 1024});
    t.on_fill(make_fill(1000, "A", order_side::buy, 100.0));
    t.on_fill(make_fill(1500, "B", order_side::buy, 200.0));
    t.on_fill(make_fill(2000, "A", order_side::buy, 100.0));

    // Mark A at t=20s. B's two-sibling-spanned fill should not be touched.
    t.on_mark("A", 101.0, epoch_ms(20'000));
    EXPECT_EQ(t.sample_count(), 2u);
    EXPECT_EQ(t.pending_count(), 1u);        // the B fill

    // Mark B — now drain it.
    t.on_mark("B", 202.0, epoch_ms(20'000));
    EXPECT_EQ(t.sample_count(), 3u);
    EXPECT_EQ(t.pending_count(), 0u);
}

TEST(AdverseSelection, MaxPendingOverflow_DropsOldestAndCountsIt)
{
    AdverseSelectionTracker t({10s, 3});   // cap = 3 so overflow fires fast
    t.on_fill(make_fill(1000, "X", order_side::buy, 100.0));
    t.on_fill(make_fill(1001, "X", order_side::buy, 100.0));
    t.on_fill(make_fill(1002, "X", order_side::buy, 100.0));
    EXPECT_EQ(t.pending_count(), 3u);
    EXPECT_EQ(t.dropped_count(), 0u);

    t.on_fill(make_fill(1003, "X", order_side::buy, 100.0));
    EXPECT_EQ(t.pending_count(), 3u);         // still capped
    EXPECT_EQ(t.dropped_count(), 1u);         // the first one got kicked
}

TEST(AdverseSelection, ZeroOrNegativeMid_Skipped)
{
    AdverseSelectionTracker t({10s, 1024});
    t.on_fill(make_fill(1000, "X", order_side::buy, 100.0));
    t.on_mark("X", 0.0,  epoch_ms(20'000));   // bad mid
    t.on_mark("X", -1.0, epoch_ms(20'000));   // bad mid
    EXPECT_EQ(t.sample_count(), 0u);
    EXPECT_EQ(t.pending_count(), 1u);

    t.on_mark("X", 101.0, epoch_ms(20'000));   // valid → drained
    EXPECT_EQ(t.sample_count(), 1u);
}

TEST(AdverseSelection, StdevNonZeroForDivergentMarkouts)
{
    AdverseSelectionTracker t({10s, 1024});
    t.on_fill(make_fill(1000, "X", order_side::buy, 100.0));
    t.on_fill(make_fill(1001, "X", order_side::buy, 100.0));
    // Drain both at same mark but at different prices — need two passes.
    t.on_mark("X", 101.0, epoch_ms(20'000));   // +100 bps for both
    // Welford stdev of [100, 100] with sample stdev = 0
    EXPECT_NEAR(t.stdev_bps(), 0.0, 1e-6);

    // Now one more fill and a different mark.
    t.on_fill(make_fill(30'000, "X", order_side::buy, 100.0));
    t.on_mark("X", 99.0, epoch_ms(50'000));    // -100 bps
    EXPECT_EQ(t.sample_count(), 3u);
    // mean = (100 + 100 - 100) / 3 = 33.333...
    EXPECT_NEAR(t.mean_bps(), 33.333333333, 1e-3);
    // Sample variance = ((100-33.33)^2 + (100-33.33)^2 + (-100-33.33)^2) / 2
    //                 = (4444.44 + 4444.44 + 17777.78) / 2 = 13333.33
    // stdev = sqrt(13333.33) ≈ 115.47
    EXPECT_NEAR(t.stdev_bps(), 115.47, 0.1);
}

TEST(AdverseSelection, ResetClearsEverything)
{
    AdverseSelectionTracker t({10s, 1024});
    t.on_fill(make_fill(1000, "X", order_side::buy, 100.0));
    t.on_mark("X", 101.0, epoch_ms(15'000));
    EXPECT_EQ(t.sample_count(), 1u);

    t.reset();
    EXPECT_EQ(t.sample_count(), 0u);
    EXPECT_EQ(t.pending_count(), 0u);
    EXPECT_EQ(t.dropped_count(), 0u);
    EXPECT_DOUBLE_EQ(t.mean_bps(), 0.0);
}
