#include <gtest/gtest.h>

#include "execution/order_tracker.h"

#include <atomic>
#include <thread>
#include <vector>

TEST(OrderTracker, ActiveCountTracksLifecycleTransitionsExactly)
{
    OrderTracker tracker;

    EXPECT_EQ(tracker.active_count(), 0u);
    tracker.set_status(1, order_status::rejected);
    EXPECT_EQ(tracker.active_count(), 0u);

    tracker.set_status(2, order_status::pending);
    EXPECT_EQ(tracker.active_count(), 1u);
    tracker.set_status(2, order_status::open);
    tracker.set_status(2, order_status::partially_filled);
    EXPECT_EQ(tracker.active_count(), 1u);

    tracker.set_status(2, order_status::filled);
    EXPECT_EQ(tracker.active_count(), 0u);
    tracker.set_status(2, order_status::filled);
    tracker.set_status(1, order_status::cancelled);
    EXPECT_EQ(tracker.active_count(), 0u);
}

TEST(OrderTracker, ActiveCountSupportsIndependentOrdersAndReset)
{
    OrderTracker tracker;
    tracker.set_status(1, order_status::pending);
    tracker.set_status(2, order_status::open);
    tracker.set_status(3, order_status::partially_filled);
    EXPECT_EQ(tracker.active_count(), 3u);

    tracker.set_status(2, order_status::cancelled);
    EXPECT_EQ(tracker.active_count(), 2u);
    tracker.reset();
    EXPECT_EQ(tracker.active_count(), 0u);
    EXPECT_TRUE(tracker.get_open_orders().empty());
}

TEST(OrderTracker, ActiveCountAtomicSupportsConcurrentReaders)
{
    OrderTracker tracker;
    constexpr std::size_t order_count = 64;
    std::atomic<bool> writer_done{false};
    std::atomic<bool> out_of_range{false};
    std::vector<std::thread> readers;

    for (int i = 0; i < 4; ++i)
    {
        readers.emplace_back([&] {
            while (!writer_done.load(std::memory_order_acquire))
            {
                if (tracker.active_count_atomic().load(std::memory_order_acquire) > order_count)
                    out_of_range.store(true, std::memory_order_release);
            }
        });
    }

    for (std::size_t id = 1; id <= order_count; ++id)
        tracker.set_status(id, order_status::pending);
    for (std::size_t id = 1; id <= order_count; ++id)
        tracker.set_status(id, order_status::filled);

    writer_done.store(true, std::memory_order_release);
    for (auto& reader : readers) reader.join();

    EXPECT_FALSE(out_of_range.load(std::memory_order_acquire));
    EXPECT_EQ(tracker.active_count_atomic().load(std::memory_order_acquire), 0u);
}
