#include <gtest/gtest.h>
#include "types/object_pool.h"
#include "types/pool_exhausted.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <semaphore>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

TEST(ObjectPool, WorkerReleaseReturnsDirectlyToFreeStack)
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

    EXPECT_EQ(pool.deferred_pending(), 0u);
    EXPECT_EQ(pool.in_use(), 0u);

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

TEST(ObjectPool, ConcurrentAcquireConsumersReceiveDistinctSlots)
{
    constexpr std::size_t per_consumer = 64;
    ObjectPool<Widget, per_consumer * 2> pool;
    pool.set_forbid_runtime_grow(true);

    std::array<std::shared_ptr<Widget>, per_consumer> first;
    std::array<std::shared_ptr<Widget>, per_consumer> second;
    std::barrier start{3};

    std::thread a([&] {
        start.arrive_and_wait();
        for (std::size_t i = 0; i < per_consumer; ++i)
            first[i] = pool.acquire(static_cast<int>(i), 1.0);
    });
    std::thread b([&] {
        start.arrive_and_wait();
        for (std::size_t i = 0; i < per_consumer; ++i)
            second[i] = pool.acquire(static_cast<int>(i + per_consumer), 2.0);
    });

    start.arrive_and_wait();
    a.join();
    b.join();

    std::set<Widget*> slots;
    for (const auto& value : first)
        ASSERT_TRUE(slots.insert(value.get()).second);
    for (const auto& value : second)
        ASSERT_TRUE(slots.insert(value.get()).second);
    EXPECT_EQ(pool.in_use(), per_consumer * 2);
}

// Test with a type that has non-trivial construction (std::string member)
struct StringWidget
{
    std::string name;
    int value;
    StringWidget(const std::string& n, int v) : name(n), value(v) {}
};

namespace {

struct GatedPoolProbe
{
    std::binary_semaphore* entered;
    std::binary_semaphore* resume;
    std::atomic<unsigned>* destructors;
    volatile std::uint32_t sentinel{0};

    GatedPoolProbe(std::binary_semaphore* entered_,
                   std::binary_semaphore* resume_,
                   std::atomic<unsigned>* destructors_)
        : entered(entered_), resume(resume_), destructors(destructors_) {}

    ~GatedPoolProbe()
    {
        auto* const entered_gate = entered;
        auto* const resume_gate = resume;
        auto* const destructor_count = destructors;
        entered_gate->release();
        resume_gate->acquire();

        // Deliberately touch object storage after the gate. A safe pool-state
        // owner keeps this slot alive while the destructor is in flight.
        sentinel = 0xC0FFEEu;
        destructor_count->fetch_add(1, std::memory_order_relaxed);
    }
};

struct ThreeWordProbe
{
    std::uint32_t words[3]{};
};

static_assert(sizeof(ThreeWordProbe) == 3 * sizeof(std::uint32_t));

struct MaybeThrowProbe
{
    explicit MaybeThrowProbe(bool should_throw)
    {
        if (should_throw)
            throw std::runtime_error("constructor failure");
    }
};

struct NonTrivialLifetimeProbe
{
    std::string value;
    std::atomic<unsigned>* destructors;

    NonTrivialLifetimeProbe(std::string value_,
                            std::atomic<unsigned>* destructors_)
        : value(std::move(value_)), destructors(destructors_) {}

    ~NonTrivialLifetimeProbe()
    {
        destructors->fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace

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

TEST(ObjectPool, SlotStridePreservesFreeNodeAlignment)
{
    ObjectPool<ThreeWordProbe, 4> pool;
    std::array<std::shared_ptr<ThreeWordProbe>, 4> held;

    for (auto& value : held)
    {
        value = pool.acquire();
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(value.get()) %
                      alignof(void*),
                  0u);
    }

    for (auto& value : held)
        value.reset();
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(ObjectPool, ThrowingConstructorReturnsSlotAndAccounting)
{
    ObjectPool<MaybeThrowProbe, 1> pool;
    pool.set_forbid_runtime_grow(true);

    EXPECT_THROW((void)pool.acquire(true), std::runtime_error);
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_EQ(pool.grow_count(), 0u);

    auto value = pool.acquire(false);
    EXPECT_NE(value, nullptr);
    EXPECT_EQ(pool.in_use(), 1u);
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
// Direct safety tests for escaped shared_ptrs. Pool State remains owned by
// every live Returner, so objects stay valid through their last strong drop.
// ---------------------------------------------------------------------------

TEST(ObjectPool, LateDropAfterDtorIsSafe)
{
    std::shared_ptr<Widget> survivor;
    {
        ObjectPool<Widget, 4> pool;
        survivor = pool.acquire(123, 4.56);

        // Also return one from the temporary strong owner.
        auto temp = pool.acquire(7, 7.7);
        temp.reset();
    }
    // The facade is gone, but survivor owns the backing State through its
    // Returner and remains a valid strong shared_ptr.
    EXPECT_EQ(survivor->x, 123);
    EXPECT_DOUBLE_EQ(survivor->y, 4.56);

    survivor.reset();
    SUCCEED();
}

TEST(ObjectPool, LateDropNonTrivialAfterDtorIsSafe)
{
    std::atomic<unsigned> destructors{0};
    std::shared_ptr<NonTrivialLifetimeProbe> survivor;
    {
        ObjectPool<NonTrivialLifetimeProbe, 4> pool;
        survivor = pool.acquire("late-drop-target", &destructors);

        // one additional direct return
        auto temp = pool.acquire("deferred-late", &destructors);
        temp.reset();
    }

    EXPECT_EQ(survivor->value, "late-drop-target");
    EXPECT_EQ(destructors.load(std::memory_order_relaxed), 1u);
    survivor.reset();
    EXPECT_EQ(destructors.load(std::memory_order_relaxed), 2u);
}

TEST(ObjectPool, ConcurrentLastDropDoesNotTouchReusedOwner)
{
    using Pool = ObjectPool<GatedPoolProbe, 1>;

    alignas(Pool) std::array<std::byte, sizeof(Pool)> owner_storage{};
    auto* first = std::construct_at(
        reinterpret_cast<Pool*>(owner_storage.data()));

    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
    std::atomic<unsigned> destructors{0};
    auto held = first->acquire(&entered, &resume, &destructors);

    std::thread dropper([owned = std::move(held)]() mutable {
        owned.reset();
    });

    // The Returner has already admitted the old pool and is blocked inside
    // T::~T(). Replace the facade at the same address before it continues.
    if (!entered.try_acquire_for(std::chrono::seconds(5)))
    {
        // Keep the test failure bounded even if a future regression prevents
        // the destructor from reaching its synchronization point.
        resume.release();
        dropper.join();
        FAIL() << "last-drop destructor did not reach the synchronization point";
        return;
    }
    std::destroy_at(first);
    auto* replacement = std::construct_at(
        reinterpret_cast<Pool*>(owner_storage.data()));
    resume.release();
    dropper.join();

    EXPECT_EQ(destructors.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(replacement->in_use(), 0u)
        << "late return must target the old backing state, not a reused facade";
    std::destroy_at(replacement);
}

TEST(ObjectPool, RearmForReuseRestoresPoolAfterSimulatedMCReset)
{
    ObjectPool<Widget, 8> pool;

    auto old_trial_handle = pool.acquire(1, 1.0);
    EXPECT_EQ(pool.in_use(), 1u);

    // Simulate the MC reset seam. An escaped old-trial handle remains valid
    // and keeps its slot reserved until it returns; it must not be leaked.
    pool.rearm_for_reuse();
    EXPECT_EQ(old_trial_handle->x, 1);
    old_trial_handle.reset();
    EXPECT_EQ(pool.in_use(), 0u);

    auto w2 = pool.acquire(42, 2.0);
    EXPECT_EQ(w2->x, 42);

    auto w3 = pool.acquire(43, 3.0);
    EXPECT_NE(w2.get(), w3.get());
}

TEST(ObjectPool, DirectReturnsNeedNoDeferredDrain)
{
    ObjectPool<Widget, 4> pool;
    auto w = pool.acquire(9, 9.0);
    w.reset();
    EXPECT_EQ(pool.deferred_pending(), 0u);

    // Existing engine cleanup call sites may still invoke this method; it is
    // intentionally harmless after direct MPSC returns.
    pool.drain_deferred_returns();
    EXPECT_EQ(pool.deferred_pending(), 0u);
}
