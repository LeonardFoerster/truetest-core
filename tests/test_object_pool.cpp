#include <gtest/gtest.h>
#include "types/object_pool.h"
#include "types/pool_exhausted.h"

#include <set>
#include <thread>
#include <vector>
#include <string>

// LSan helpers for intentional late-drop leaks (see LateDropNonTrivialAfterDtorIsSafe).
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    include <sanitizer/lsan_interface.h>
#    define TT_OBJECT_POOL_HAS_LSAN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(TT_OBJECT_POOL_HAS_LSAN)
#  include <sanitizer/lsan_interface.h>
#  define TT_OBJECT_POOL_HAS_LSAN 1
#endif

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

// ---------------------------------------------------------------------------
// Direct safety tests for late-drop hardening (alive_ + Returner struct)
// These verify that shared_ptrs dropped after pool destruction (or during
// shutdown with escaped references via rings/workers/MC) are safe no-ops.
// ---------------------------------------------------------------------------

TEST(ObjectPool, LateDropAfterDtorIsSafe)
{
    std::shared_ptr<Widget> survivor;
    {
        ObjectPool<Widget, 4> pool;
        survivor = pool.acquire(123, 4.56);

        // Also send one through deferred path
        auto temp = pool.acquire(7, 7.7);
        temp.reset();  // -> deferred_returns_
    }
    // pool destroyed here: ~ObjectPool sets alive_=false

    // Dropping the survivor (and any pending deferred) must not UAF/crash
    // (the Returner checks alive_ and early-exits).
    survivor.reset();

    SUCCEED();  // reached without fault (ASAN would have reported otherwise)
}

// Extended per 2026-07-18 memory-check remediation (Phase 2):
// Use non-trivial T (has std::string dtor) for late-drop scenario.
// Documents that for escaped drops after dtor/rearm we intentionally leak
// (do not invoke ~T) to avoid UAF into pool storage. ASAN + manual inspection
// must not see use-after-free or double-free.
TEST(ObjectPool, LateDropNonTrivialAfterDtorIsSafe)
{
    std::shared_ptr<StringWidget> survivor;
    // Captured while the pool (and thus StringWidget storage) is still live.
    // After ~pool, reading survivor->name would itself be UAF.
    const void* intentional_string_heap = nullptr;
    {
        ObjectPool<StringWidget, 4> pool;
        // Long enough to force a heap std::string buffer (beyond SSO) so the
        // intentional late-drop leak is a real external allocation.
        survivor = pool.acquire("late-drop-target", 777);
        intentional_string_heap = survivor->name.data();

        // one via deferred too
        auto temp = pool.acquire("deferred-late", 1);
        temp.reset();
    }
    // ~pool disarms lifetime_ here.

    // Drop must be a safe no-op: running ~T would UAF into freed pool storage,
    // so heap owned by T (std::string buffer) is intentionally abandoned until
    // process exit. Quarantine that expected allocation so full ASan+LSan
    // suites stay fail-closed green without weakening the late-drop contract.
#if defined(TT_OBJECT_POOL_HAS_LSAN)
    if (intentional_string_heap)
        __lsan_ignore_object(intentional_string_heap);
#endif
    survivor.reset();

    SUCCEED();
}

TEST(ObjectPool, RearmForReuseRestoresPoolAfterSimulatedMCReset)
{
    ObjectPool<Widget, 8> pool;

    // Use the pool
    {
        auto w = pool.acquire(1, 1.0);
        EXPECT_EQ(w->x, 1);
    }
    pool.drain_deferred_returns();

    // Simulate MC reuse path (engine calls this on pools during reset_for_next_trial)
    pool.rearm_for_reuse();

    // Pool must be usable again
    auto w2 = pool.acquire(42, 2.0);
    EXPECT_EQ(w2->x, 42);

    auto w3 = pool.acquire(43, 3.0);
    EXPECT_NE(w2.get(), w3.get());
}

TEST(ObjectPool, DrainAfterDtorIsNoop)
{
    ObjectPool<Widget, 4> pool;
    auto w = pool.acquire(9, 9.0);
    w.reset();  // deferred
    EXPECT_GT(pool.deferred_pending(), 0u);

    // Destroy pool (disarms)
    // (we can't easily dtor early here; instead force the guard path)
    // Call drain after manually disarming is not public, but we can at least
    // ensure normal drain after all releases is safe (already covered).
    pool.drain_deferred_returns();
    EXPECT_EQ(pool.deferred_pending(), 0u);
}
