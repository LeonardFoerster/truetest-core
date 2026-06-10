#include <gtest/gtest.h>

#include "helpers/alloc_counter.h"
#include "types/control_block_pool.h"
#include "types/object_pool.h"

#include <vector>

struct PodEvent
{
    int value = 0;
    explicit PodEvent(int v = 0) : value(v) {}
};

TEST(ControlBlockPool, AcquireReleaseCycles)
{
    ControlBlockPool cb;
    void* a = cb.acquire_slot();
    void* b = cb.acquire_slot();
    EXPECT_NE(a, b);
    cb.release_slot(a);
    cb.release_slot(b);
    EXPECT_EQ(cb.grow_count(), 0u);
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

    truetest::test::alloc::reset();

    {
        truetest::test::alloc::measure_window window;
        std::vector<std::shared_ptr<PodEvent>> held;
        held.reserve(32);
        for (int i = 0; i < 32; ++i)
            held.push_back(pool.acquire(i));
        const auto snap = window.total();
        // Pooled control blocks: no per-acquire heap traffic (tiny allowance
        // for libstdc++ one-time TLS / exception tables on first use).
        EXPECT_LE(snap.count, 5u) << "heap allocs=" << snap.count
                                  << " bytes=" << snap.bytes;
        EXPECT_LE(snap.bytes, 512u) << "heap bytes=" << snap.bytes;
    }

    EXPECT_EQ(cb.in_use(), 0u);
    EXPECT_EQ(pool.in_use(), 0u);
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