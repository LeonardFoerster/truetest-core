#include <gtest/gtest.h>

#include "types/deferred_return_queue.h"

#include <thread>
#include <vector>

TEST(DeferredReturnQueue, SingleProducerConsumer)
{
    DeferredReturnQueue<256> q;
    int a = 1;
    int b = 2;
    EXPECT_TRUE(q.try_push(&a));
    EXPECT_TRUE(q.try_push(&b));
    EXPECT_EQ(q.pending(), 2u);

    void* out = nullptr;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, &a);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, &b);
    EXPECT_FALSE(q.try_pop(out));
}

TEST(DeferredReturnQueue, MultiProducerSingleConsumer)
{
    DeferredReturnQueue<4096> q;
    constexpr int threads = 4;
    constexpr int per_thread = 2000;

    std::vector<int> values(static_cast<std::size_t>(threads * per_thread));
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = static_cast<int>(i);

    std::vector<std::thread> producers;
    for (int t = 0; t < threads; ++t)
    {
        producers.emplace_back([&, t]() {
            const int base = t * per_thread;
            for (int i = 0; i < per_thread; ++i)
            {
                while (!q.try_push(&values[static_cast<std::size_t>(base + i)]))
                {
                }
            }
        });
    }

    std::size_t popped = 0;
    void* out = nullptr;
    while (popped < static_cast<std::size_t>(threads * per_thread))
    {
        if (q.try_pop(out))
            ++popped;
    }

    for (auto& th : producers)
        th.join();

    EXPECT_EQ(popped, static_cast<std::size_t>(threads * per_thread));
    EXPECT_EQ(q.pending(), 0u);
}