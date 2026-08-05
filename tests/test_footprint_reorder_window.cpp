#include <gtest/gtest.h>

#include "analytics/footprint/footprint_reorder_window.h"

using namespace truetest::footprint;

namespace {
constexpr std::int64_t kSecond = 1'000'000'000LL;

PublicTrade make_trade(std::int64_t event_ns, std::uint64_t obs_seq)
{
    PublicTrade t;
    t.event_ns = event_ns;
    t.obs_seq = obs_seq;
    return t;
}
} // namespace

TEST(FootprintReorderWindow, AcceptsMonotonicArrivals)
{
    ReorderWindow w(2 * kSecond);
    EXPECT_TRUE(w.offer(make_trade(0, 0)));
    EXPECT_TRUE(w.offer(make_trade(kSecond, 1)));
    EXPECT_TRUE(w.offer(make_trade(2 * kSecond, 2)));
}

TEST(FootprintReorderWindow, ReordersOutOfOrderArrivalsWithinWindow)
{
    ReorderWindow w(2 * kSecond);
    w.offer(make_trade(3 * kSecond, 2)); // arrives first but is "later"
    w.offer(make_trade(1 * kSecond, 0)); // arrives second but is "earlier"
    w.offer(make_trade(2 * kSecond, 1));

    // Nothing is safe yet - watermark=3s, threshold=1s, and the trade AT
    // 1s is <= threshold so it drains; 2s and 3s are still held.
    std::vector<PublicTrade> out;
    w.drain_ready(out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].event_ns, kSecond);
}

TEST(FootprintReorderWindow, DrainReadyEmitsInAscendingEventNsOrder)
{
    ReorderWindow w(10 * kSecond);
    w.offer(make_trade(5 * kSecond, 0));
    w.offer(make_trade(1 * kSecond, 1));
    w.offer(make_trade(3 * kSecond, 2));
    w.offer(make_trade(2 * kSecond, 3));

    std::vector<PublicTrade> out;
    w.flush(out); // force-emit everything regardless of watermark
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0].event_ns, kSecond);
    EXPECT_EQ(out[1].event_ns, 2 * kSecond);
    EXPECT_EQ(out[2].event_ns, 3 * kSecond);
    EXPECT_EQ(out[3].event_ns, 5 * kSecond);
}

TEST(FootprintReorderWindow, RejectsArrivalOlderThanWindow)
{
    ReorderWindow w(2 * kSecond);
    w.offer(make_trade(10 * kSecond, 0)); // watermark now 10s
    // 10s - 2s = 8s threshold; anything strictly older than 8s is rejected.
    EXPECT_FALSE(w.offer(make_trade(7 * kSecond, 1)));
    EXPECT_TRUE(w.offer(make_trade(8 * kSecond, 2))); // exactly at the edge - still accepted
}

TEST(FootprintReorderWindow, RejectedTradeIsNotBuffered)
{
    ReorderWindow w(2 * kSecond);
    w.offer(make_trade(10 * kSecond, 0));
    const auto size_before = w.size();
    EXPECT_FALSE(w.offer(make_trade(0, 99)));
    EXPECT_EQ(w.size(), size_before); // rejection must not silently buffer it anyway
}

TEST(FootprintReorderWindow, DrainReadyIsIdempotentWhenNothingIsReady)
{
    ReorderWindow w(2 * kSecond);
    w.offer(make_trade(kSecond, 0));
    std::vector<PublicTrade> out;
    w.drain_ready(out);
    EXPECT_TRUE(out.empty()); // watermark=1s, threshold=-1s, 1s > threshold -> not ready
    EXPECT_EQ(w.size(), 1u);
}

TEST(FootprintReorderWindow, FlushClearsTheBuffer)
{
    ReorderWindow w(2 * kSecond);
    w.offer(make_trade(kSecond, 0));
    w.offer(make_trade(2 * kSecond, 1));
    std::vector<PublicTrade> out;
    w.flush(out);
    EXPECT_EQ(out.size(), 2u);
    EXPECT_TRUE(w.empty());
}

TEST(FootprintReorderWindow, WatermarkTracksHighestEventNsSeen)
{
    ReorderWindow w(2 * kSecond);
    w.offer(make_trade(5 * kSecond, 0));
    w.offer(make_trade(3 * kSecond, 1)); // lower - must not move the watermark backward
    EXPECT_EQ(w.watermark_ns(), 5 * kSecond);
}
