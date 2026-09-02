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

TEST(OrderIdGenerator, DeterministicScopeIsThreadLocalAndRestoresGlobalSequence)
{
    OrderIdGenerator::reset(500);
    EXPECT_EQ(OrderIdGenerator::next(), 500U);
    {
        OrderIdGenerator::deterministic_scope trial_scope;
        EXPECT_EQ(OrderIdGenerator::next(), 1U);
        EXPECT_EQ(OrderIdGenerator::next(), 2U);
        {
            OrderIdGenerator::deterministic_scope nested(100);
            EXPECT_EQ(OrderIdGenerator::next(), 100U);
        }
        EXPECT_EQ(OrderIdGenerator::next(), 3U);
    }
    EXPECT_EQ(OrderIdGenerator::next(), 501U);
}

TEST(OrderIdGenerator, ParallelDeterministicScopesDoNotShareScheduleState)
{
    constexpr int thread_count = 4;
    std::vector<std::vector<std::uint64_t>> results(thread_count);
    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index)
    {
        workers.emplace_back([&, thread_index] {
            OrderIdGenerator::deterministic_scope trial_scope;
            for (std::uint64_t expected = 1; expected <= 100; ++expected)
                results[static_cast<std::size_t>(thread_index)].push_back(
                    OrderIdGenerator::next());
        });
    }
    for (auto& worker : workers)
        worker.join();
    for (const auto& ids : results)
    {
        ASSERT_EQ(ids.size(), 100U);
        for (std::size_t i = 0; i < ids.size(); ++i)
            EXPECT_EQ(ids[i], static_cast<std::uint64_t>(i + 1));
    }
}
