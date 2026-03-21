#include <gtest/gtest.h>
#include "types/object_pool.h"

#include <set>
#include <thread>
#include <vector>
#include <string>

// Simple test type
struct Widget
{
    int x;
    double y;
    Widget() : x(0), y(0.0) {}
    Widget(int x_, double y_) : x(x_), y(y_) {}
};

TEST(ObjectPool, AcquireAndRelease)
{
    ObjectPool<Widget, 16> pool;
    auto w = pool.acquire(42, 3.14);
    EXPECT_EQ(w->x, 42);
    EXPECT_DOUBLE_EQ(w->y, 3.14);
}

TEST(ObjectPool, ReusesMemory)
{
    ObjectPool<Widget, 16> pool;
    Widget* raw1;
    {
        auto w = pool.acquire(1, 1.0);
        raw1 = w.get();
    }
    // After release, next acquire should reuse the same slot
    auto w2 = pool.acquire(2, 2.0);
    EXPECT_EQ(w2.get(), raw1);
    EXPECT_EQ(w2->x, 2);
}

TEST(ObjectPool, ExhaustionTriggersNewBlock)
{
    ObjectPool<Widget, 4> pool;  // tiny blocks of 4
    EXPECT_EQ(pool.block_count(), 1u);

    // Hold onto all 4 slots
    std::vector<std::shared_ptr<Widget>> held;
    for (int i = 0; i < 4; ++i)
        held.push_back(pool.acquire(i, 0.0));

    EXPECT_EQ(pool.block_count(), 1u);

    // 5th acquire should trigger a new block
    held.push_back(pool.acquire(4, 0.0));
    EXPECT_EQ(pool.block_count(), 2u);

    // Verify all are distinct and valid
    std::set<Widget*> ptrs;
    for (auto& w : held)
        ptrs.insert(w.get());
    EXPECT_EQ(ptrs.size(), 5u);
}

TEST(ObjectPool, DestructorCalled)
{
    static int dtor_count = 0;
    struct Counted
    {
        ~Counted() { dtor_count++; }
    };

    dtor_count = 0;
    ObjectPool<Counted, 16> pool;
    {
        auto c = pool.acquire();
    }
    EXPECT_EQ(dtor_count, 1);

    {
        auto c1 = pool.acquire();
        auto c2 = pool.acquire();
    }
    EXPECT_EQ(dtor_count, 3);
}

TEST(ObjectPool, MultipleAcquiresDistinct)
{
    ObjectPool<Widget, 16> pool;
    auto w1 = pool.acquire(1, 1.0);
    auto w2 = pool.acquire(2, 2.0);
    auto w3 = pool.acquire(3, 3.0);

    EXPECT_NE(w1.get(), w2.get());
    EXPECT_NE(w2.get(), w3.get());
    EXPECT_NE(w1.get(), w3.get());

    EXPECT_EQ(w1->x, 1);
    EXPECT_EQ(w2->x, 2);
    EXPECT_EQ(w3->x, 3);
}

TEST(ObjectPool, ThreadSafety)
{
    ObjectPool<Widget, 64> pool;
    constexpr int threads = 4;
    constexpr int ops_per_thread = 10000;

    std::atomic<int> total_acquired{0};

    auto worker = [&]() {
        for (int i = 0; i < ops_per_thread; ++i)
        {
            auto w = pool.acquire(i, static_cast<double>(i));
            EXPECT_EQ(w->x, i);
            total_acquired.fetch_add(1, std::memory_order_relaxed);
            // shared_ptr goes out of scope, releasing back to pool
        }
    };

    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t)
        workers.emplace_back(worker);

    for (auto& t : workers)
        t.join();

    EXPECT_EQ(total_acquired.load(), threads * ops_per_thread);
}

TEST(ObjectPool, SharedPtrKeepsAlive)
{
    ObjectPool<Widget, 16> pool;
    std::shared_ptr<Widget> copy;
    {
        auto w = pool.acquire(99, 0.5);
        copy = w;  // second reference
    }
    // Original shared_ptr is gone, but copy still holds the object
    EXPECT_EQ(copy->x, 99);
    EXPECT_DOUBLE_EQ(copy->y, 0.5);
}

// Test with a type that has non-trivial construction (std::string member)
struct StringWidget
{
    std::string name;
    int value;
    StringWidget(const std::string& n, int v) : name(n), value(v) {}
};

TEST(ObjectPool, NonTrivialType)
{
    ObjectPool<StringWidget, 8> pool;
    auto w = pool.acquire("hello", 42);
    EXPECT_EQ(w->name, "hello");
    EXPECT_EQ(w->value, 42);

    // Release and reacquire — string destructor must have been called
    w.reset();
    auto w2 = pool.acquire("world", 99);
    EXPECT_EQ(w2->name, "world");
    EXPECT_EQ(w2->value, 99);
}
