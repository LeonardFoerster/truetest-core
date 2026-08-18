#include <gtest/gtest.h>
#include "types/order_id.h"
#include <set>
#include <thread>
#include <vector>

TEST(OrderIdGenerator, Monotonic)
{
    OrderIdGenerator::reset(1);
    uint64_t prev = OrderIdGenerator::next();
    for (int i = 0; i < 1000; ++i)
    {
        uint64_t cur = OrderIdGenerator::next();
        EXPECT_GT(cur, prev);
        prev = cur;
    }
}

TEST(OrderIdGenerator, NoDuplicates)
{
    OrderIdGenerator::reset(1);
    std::set<uint64_t> ids;
    for (int i = 0; i < 100000; ++i)
    {
        auto [it, inserted] = ids.insert(OrderIdGenerator::next());
        EXPECT_TRUE(inserted);
    }
}

TEST(OrderIdGenerator, ThreadSafe)
{
    OrderIdGenerator::reset(1);
    constexpr int threads = 4;
    constexpr int per_thread = 10000;

    std::vector<std::vector<uint64_t>> results(threads);
    std::vector<std::thread> pool;

    for (int t = 0; t < threads; ++t)
    {
        pool.emplace_back([&results, t]() {
            results[t].reserve(per_thread);
            for (int i = 0; i < per_thread; ++i)
                results[t].push_back(OrderIdGenerator::next());
        });
    }
    for (auto& th : pool) th.join();

    std::set<uint64_t> all;
    for (const auto& v : results)
        for (uint64_t id : v)
            all.insert(id);

    EXPECT_EQ(all.size(), threads * per_thread);
}

TEST(OrderIdGenerator, Reset)
{
    OrderIdGenerator::reset(100);
    EXPECT_EQ(OrderIdGenerator::next(), 100u);
    EXPECT_EQ(OrderIdGenerator::next(), 101u);
}

TEST(OrderIdGenerator, AdvanceToAtLeastNeverMovesBackward)
{
    OrderIdGenerator::reset(1);
    EXPECT_TRUE(OrderIdGenerator::advance_to_at_least(500));
    EXPECT_EQ(OrderIdGenerator::next(), 500u);

    // A stale recovery scan cannot reuse an identity allocated after its
    // snapshot was taken.
    EXPECT_EQ(OrderIdGenerator::next(), 501u);
    EXPECT_TRUE(OrderIdGenerator::advance_to_at_least(100));
    EXPECT_EQ(OrderIdGenerator::next(), 502u);
    EXPECT_FALSE(OrderIdGenerator::advance_to_at_least(0));
}
