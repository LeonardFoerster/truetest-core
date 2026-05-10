#include <gtest/gtest.h>
#include "execution/queue_position_model.h"

#include <chrono>

using tp = std::chrono::system_clock::time_point;
using ms = std::chrono::milliseconds;

static tp at(int64_t ms_since_epoch)
{
    return tp(ms(ms_since_epoch));
}

TEST(L2SnapshotQueueModel, ReturnsLevelSizeAtOurPrice)
{
    L2SnapshotQueueModel m;
    m.on_snapshot("BTC", {{100.0, 5.0}, {99.5, 10.0}}, {{100.5, 3.0}, {101.0, 8.0}});
    EXPECT_DOUBLE_EQ(m.queue_ahead("BTC", order_side::buy,  100.0, at(0)), 5.0);
    EXPECT_DOUBLE_EQ(m.queue_ahead("BTC", order_side::buy,  99.5,  at(0)), 10.0);
    EXPECT_DOUBLE_EQ(m.queue_ahead("BTC", order_side::sell, 100.5, at(0)), 3.0);
    EXPECT_DOUBLE_EQ(m.queue_ahead("BTC", order_side::sell, 101.0, at(0)), 8.0);
}

// No level at our price (improving the BBO or sitting alone at a deeper
// price) → queue_ahead = 0, the legacy fill-on-cross path takes over.
TEST(L2SnapshotQueueModel, NoLevelMeansZeroQueue)
{
    L2SnapshotQueueModel m;
    m.on_snapshot("BTC", {{100.0, 5.0}}, {{100.5, 3.0}});
    EXPECT_DOUBLE_EQ(m.queue_ahead("BTC", order_side::buy,  100.5, at(0)), 0.0);
    EXPECT_DOUBLE_EQ(m.queue_ahead("BTC", order_side::buy,  99.0,  at(0)), 0.0);
    EXPECT_DOUBLE_EQ(m.queue_ahead("BTC", order_side::sell, 100.0, at(0)), 0.0);
}

// on_update overwrites the level size; size=0 removes the level so it
// reverts to the no-level case.
TEST(L2SnapshotQueueModel, UpdateOverwritesAndRemoves)
{
    L2SnapshotQueueModel m;
    m.on_snapshot("BTC", {{100.0, 5.0}}, {});

    m.on_update("BTC", order_side::buy, 100.0, 2.0);
    EXPECT_DOUBLE_EQ(m.queue_ahead("BTC", order_side::buy, 100.0, at(0)), 2.0);

    m.on_update("BTC", order_side::buy, 100.0, 0.0);
    EXPECT_DOUBLE_EQ(m.queue_ahead("BTC", order_side::buy, 100.0, at(0)), 0.0);
}

// Stale snapshots (> max_staleness) refuse to estimate. Returning 0
// degrades to legacy behaviour rather than feeding the adapter a stale
// queue number that would silently bias fills.
TEST(L2SnapshotQueueModel, StaleSnapshotReturnsZero)
{
    L2SnapshotQueueModel m(ms(500));

    // Snapshot at t=0 (epoch) — system_clock::now() in on_snapshot is
    // wall-clock, not the test's at(...) value. Submit ts also has to
    // be wall-clock to compare against. Use real clock to seed.
    const auto t0 = std::chrono::system_clock::now();
    m.on_snapshot("BTC", {{100.0, 5.0}}, {});
    EXPECT_DOUBLE_EQ(m.queue_ahead("BTC", order_side::buy, 100.0, t0), 5.0);

    // Submit ts 2s ahead — way past 500ms staleness.
    EXPECT_DOUBLE_EQ(m.queue_ahead("BTC", order_side::buy, 100.0,
                                   t0 + std::chrono::seconds(2)), 0.0);
}

TEST(NoQueueModel, AlwaysZero)
{
    NoQueueModel m;
    m.on_snapshot("BTC", {{100.0, 5.0}}, {{100.5, 3.0}});
    m.on_update("BTC", order_side::buy, 100.0, 2.0);
    EXPECT_DOUBLE_EQ(m.queue_ahead("BTC", order_side::buy, 100.0, at(0)), 0.0);
    EXPECT_DOUBLE_EQ(m.queue_ahead("BTC", order_side::sell, 100.5, at(0)), 0.0);
}
