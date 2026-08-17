#include <gtest/gtest.h>

#include "helpers/alloc_counter.h"
#include "types/control_block_pool.h"
#include "types/object_pool.h"

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <thread>
#include <vector>

struct PodEvent
{
    int value = 0;
    explicit PodEvent(int v = 0) : value(v) {}
};

namespace {

struct LifetimeProbeState
{
    std::atomic<unsigned> destructors{0};
};

struct LifetimeProbe
{
    std::shared_ptr<LifetimeProbeState> state;
    int value{0};

    LifetimeProbe(std::shared_ptr<LifetimeProbeState> s, int v)
        : state(std::move(s)), value(v) {}

    ~LifetimeProbe()
    {
        state->destructors.fetch_add(1, std::memory_order_relaxed);
    }
};

struct alignas(128) OveralignedControlBlockPayload
{
    std::byte bytes[128];
};

} // namespace

TEST(ControlBlockPool, AcquireReleaseCycles)
{
    ControlBlockPool cb;
    void* a = cb.acquire_slot();
    void* b = cb.acquire_slot();
    EXPECT_NE(a, b);
    cb.release_slot(a);
    cb.release_slot(b);
    EXPECT_EQ(cb.grow_count(), 0u);
    EXPECT_EQ(cb.deferred_pending(), 0u);
    EXPECT_EQ(cb.deferred_overflow(), 0u);
}

TEST(ControlBlockPool, ConcurrentAcquireConsumersReceiveDistinctSlots)
{
    constexpr std::size_t per_consumer = 128;
    ControlBlockPool cb;
    cb.set_forbid_runtime_grow(true);

    std::array<void*, per_consumer> first{};
    std::array<void*, per_consumer> second{};
    std::barrier start{3};

    std::thread a([&] {
        start.arrive_and_wait();
        for (auto& slot : first)
            slot = cb.acquire_slot();
    });
    std::thread b([&] {
        start.arrive_and_wait();
        for (auto& slot : second)
            slot = cb.acquire_slot();
    });

    start.arrive_and_wait();
    a.join();
    b.join();

    std::set<void*> slots;
    for (void* slot : first)
        ASSERT_TRUE(slots.insert(slot).second);
    for (void* slot : second)
        ASSERT_TRUE(slots.insert(slot).second);
    EXPECT_EQ(cb.in_use(), per_consumer * 2);

    for (void* slot : first)
        cb.release_slot(slot);
    for (void* slot : second)
        cb.release_slot(slot);
    EXPECT_EQ(cb.in_use(), 0u);
}

TEST(ControlBlockPool, PooledSharedPtrUsesControlBlockPoolNotHeap)
{
    ControlBlockPool cb;
    cb.set_forbid_runtime_grow(true);
    cb.ensure_min_blocks(1);

    ObjectPool<PodEvent, 16> pool;
    pool.set_control_block_pool(&cb);
    pool.set_forbid_runtime_grow(true);
    pool.ensure_min_blocks(2); // 32 acquires need 2×16 slots when grow is forbidden

    std::vector<std::shared_ptr<PodEvent>> held;
    held.reserve(32);
    truetest::test::alloc::snapshot snap{};
    {
        truetest::test::alloc::measure_window window;
        for (int i = 0; i < 32; ++i)
            held.push_back(pool.acquire(i));
        snap = window.total();
    }

    EXPECT_EQ(snap.count, 0u) << "heap bytes=" << snap.bytes;
    EXPECT_EQ(snap.bytes, 0u);
    EXPECT_EQ(cb.in_use(), 32u);
    EXPECT_EQ(pool.in_use(), 32u);
    EXPECT_EQ(cb.fallback_allocations(), 0u);

    held.clear();
    EXPECT_EQ(cb.in_use(), 0u);
    EXPECT_EQ(pool.in_use(), 0u);
}

TEST(ControlBlockPool, PooledStrongAndWeakSurviveBothPoolOwners)
{
    auto probe_state = std::make_shared<LifetimeProbeState>();
    std::shared_ptr<LifetimeProbe> strong;
    std::weak_ptr<LifetimeProbe> weak;

    auto object_pool = std::make_unique<ObjectPool<LifetimeProbe, 4>>();
    auto control_pool = std::make_unique<ControlBlockPool>();
    object_pool->set_control_block_pool(control_pool.get());
    strong = object_pool->acquire(probe_state, 42);
    weak = strong;

    // Mirrors engine member destruction: control blocks currently die before
    // object pools. Both facades may disappear while external strong/weak
    // handles still own the object and its control block.
    control_pool.reset();
    object_pool.reset();

    auto locked = weak.lock();
    ASSERT_NE(locked, nullptr);
    EXPECT_EQ(locked->value, 42);
    locked.reset();

    strong.reset();
    EXPECT_EQ(probe_state->destructors.load(std::memory_order_relaxed), 1u);
    EXPECT_TRUE(weak.expired());
    weak.reset();
}

TEST(ControlBlockPool, WeakPointerRetainsControlBlockUntilFinalDrop)
{
    ControlBlockPool cb;
    ObjectPool<PodEvent, 1> pool;
    pool.set_control_block_pool(&cb);

    auto strong = pool.acquire(42);
    std::weak_ptr<PodEvent> weak = strong;
    EXPECT_EQ(cb.in_use(), 1u);

    strong.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(cb.in_use(), 1u)
        << "the control block must remain allocated while weak_ptr exists";

    weak.reset();
    EXPECT_EQ(cb.in_use(), 0u);
}

TEST(ControlBlockPool, AllocatorDoesNotReturnIntoReusedFacade)
{
    using Pool = ControlBlockPool;
    using Allocator = control_block_allocator<std::uint64_t>;

    alignas(Pool) std::array<std::byte, sizeof(Pool)> facade_storage{};
    auto* first = std::construct_at(
        reinterpret_cast<Pool*>(facade_storage.data()));
    Allocator allocator(first);
    auto* slot = allocator.allocate(1);
    std::construct_at(slot, 7u);
    std::destroy_at(slot);
    ASSERT_EQ(first->in_use(), 1u);

    std::destroy_at(first);
    auto* replacement = std::construct_at(
        reinterpret_cast<Pool*>(facade_storage.data()));
    allocator.deallocate(slot, 1);

    EXPECT_EQ(replacement->in_use(), 0u)
        << "late allocator deallocation must target the original backing State";
    std::destroy_at(replacement);
}

TEST(ControlBlockPool, OveralignedAllocatorUsesTrackedFallbackWhenPermitted)
{
    ControlBlockPool cb;
    control_block_allocator<OveralignedControlBlockPayload> allocator(&cb);

    const std::size_t before = cb.in_use();
    const std::size_t fallback_before = cb.fallback_allocations();
    auto* payload = allocator.allocate(1);
    ASSERT_NE(payload, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(payload) %
                  alignof(OveralignedControlBlockPayload),
              0u);
    EXPECT_EQ(cb.in_use(), before)
        << "oversized or over-aligned rebound types must not use a 64-byte slot";
    EXPECT_EQ(cb.fallback_allocations(), fallback_before + 1);

    allocator.deallocate(payload, 1);
    EXPECT_EQ(cb.in_use(), before);
}

TEST(ControlBlockPool, StrictOveralignedAllocatorFailsClosed)
{
    ControlBlockPool cb;
    cb.set_forbid_runtime_grow(true);
    control_block_allocator<OveralignedControlBlockPayload> allocator(&cb);

    EXPECT_THROW((void)allocator.allocate(1), pool_exhausted);
    EXPECT_EQ(cb.in_use(), 0u);
    EXPECT_EQ(cb.fallback_allocations(), 0u);
}

TEST(ControlBlockPool, AllocatorRetainsStateBeforeFirstAcquisition)
{
    ObjectPool<PodEvent, 1> pool;
    {
        ControlBlockPool cb;
        cb.set_forbid_runtime_grow(true);
        pool.set_control_block_pool(&cb);
    }

    auto value = pool.acquire(17);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->value, 17);
}

TEST(ControlBlockPool, ExhaustionRollsBackConstructedObject)
{
    ControlBlockPool cb;
    cb.set_forbid_runtime_grow(true);

    std::vector<void*> held;
    held.reserve(ControlBlockPool::slots_per_block());
    for (std::size_t i = 0; i < ControlBlockPool::slots_per_block(); ++i)
        held.push_back(cb.acquire_slot());

    auto probe_state = std::make_shared<LifetimeProbeState>();
    ObjectPool<LifetimeProbe, 1> pool;
    pool.set_control_block_pool(&cb);
    pool.set_forbid_runtime_grow(true);

    EXPECT_THROW((void)pool.acquire(probe_state, 1), pool_exhausted);
    EXPECT_EQ(probe_state->destructors.load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(pool.in_use(), 0u);
    EXPECT_EQ(pool.grow_count(), 0u);

    cb.release_slot(held.back());
    held.pop_back();
    auto value = pool.acquire(probe_state, 2);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->value, 2);
    value.reset();
    EXPECT_EQ(probe_state->destructors.load(std::memory_order_relaxed), 2u);
    EXPECT_EQ(pool.in_use(), 0u);

    for (void* slot : held)
        cb.release_slot(slot);
    EXPECT_EQ(cb.in_use(), 0u);
}

TEST(ControlBlockPool, ForbidRuntimeGrowThrows)
{
    ControlBlockPool cb;
    cb.set_forbid_runtime_grow(true);

    std::vector<void*> held;
    for (std::size_t i = 0; i < ControlBlockPool::slots_per_block(); ++i)
        held.push_back(cb.acquire_slot());

    EXPECT_THROW((void)cb.acquire_slot(), pool_exhausted);
    for (void* p : held)
        cb.release_slot(p);
}
