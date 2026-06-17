#include <gtest/gtest.h>
#include "types/object_pool.h"
#include "types/pool_exhausted.h"

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

    // 5th acquire should trigger a new block (runtime grow, not ctor)
    EXPECT_EQ(pool.grow_count(), 0u);
    held.push_back(pool.acquire(4, 0.0));
    EXPECT_EQ(pool.block_count(), 2u);
    EXPECT_EQ(pool.grow_count(), 1u);

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

// Phase 3: engine thread acquires; worker threads only release via deferred queue.
TEST(ObjectPool, DeferredReturnWorkerRelease)
{
    ObjectPool<Widget, 64> pool;
    constexpr int total = 256;
    std::vector<std::shared_ptr<Widget>> held;
    held.reserve(total);
    for (int i = 0; i < total; ++i)
        held.push_back(pool.acquire(i, static_cast<double>(i)));

    constexpr int threads = 4;
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t)
    {
        workers.emplace_back([&, t]() {
            const int chunk = total / threads;
            const int begin = t * chunk;
            const int end = (t == threads - 1) ? total : begin + chunk;
            for (int i = begin; i < end; ++i)
                held[static_cast<std::size_t>(i)].reset();
        });
    }
    for (auto& th : workers)
        th.join();

    EXPECT_GT(pool.deferred_pending(), 0u);
    pool.drain_deferred_returns();
    EXPECT_EQ(pool.deferred_pending(), 0u);

    auto w = pool.acquire(999, 1.5);
    EXPECT_EQ(w->x, 999);
}

TEST(ObjectPool, ThreadSafety)
{
    ObjectPool<Widget, 64> pool;
    constexpr int worker_threads = 4;
    constexpr int ops_per_thread = 2500;
    constexpr int total = worker_threads * ops_per_thread;

    std::vector<std::shared_ptr<Widget>> batch;
    batch.reserve(total);
    for (int i = 0; i < total; ++i)
        batch.push_back(pool.acquire(i, static_cast<double>(i)));

    std::atomic<int> released{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < worker_threads; ++t)
    {
        workers.emplace_back([&, t]() {
            const int begin = t * ops_per_thread;
            const int end = begin + ops_per_thread;
            for (int i = begin; i < end; ++i)
            {
                batch[static_cast<std::size_t>(i)].reset();
                released.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : workers)
        th.join();

    pool.drain_deferred_returns();
    EXPECT_EQ(released.load(), total);

    for (int i = 0; i < total; ++i)
    {
        auto w = pool.acquire(i, static_cast<double>(i) + 0.5);
        EXPECT_EQ(w->x, i);
    }
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

TEST(ObjectPool, ForbidRuntimeGrowThrowsWithoutGrowing)
{
    ObjectPool<Widget, 4> pool;
    pool.set_forbid_runtime_grow(true);

    std::vector<std::shared_ptr<Widget>> held;
    for (int i = 0; i < 4; ++i)
        held.push_back(pool.acquire(i, 0.0));

    EXPECT_EQ(pool.grow_count(), 0u);
    EXPECT_THROW((void)pool.acquire(99, 0.0), pool_exhausted);
    EXPECT_EQ(pool.block_count(), 1u);
}

TEST(ObjectPool, EnsureMinBlocksPreallocates)
{
    ObjectPool<Widget, 8> pool;
    EXPECT_EQ(pool.block_count(), 1u);
    pool.ensure_min_blocks(3);
    EXPECT_EQ(pool.block_count(), 3u);
    EXPECT_EQ(pool.capacity_slots(), 24u);
    EXPECT_EQ(pool.grow_count(), 0u);
}

TEST(ObjectPool, NonTrivialType)
{
    ObjectPool<StringWidget, 8> pool;
    auto w = pool.acquire("hello", 42);
    EXPECT_EQ(w->name, "hello");
    EXPECT_EQ(w->value, 42);

    // Release and reacquire - string destructor must have been called
    w.reset();
    auto w2 = pool.acquire("world", 99);
    EXPECT_EQ(w2->name, "world");
    EXPECT_EQ(w2->value, 99);
}
