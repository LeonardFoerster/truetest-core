#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

// Phase 3: MPSC deferred-return queue. Multiple producer threads (worker
// shared_ptr deleters) push freed slot pointers; a single consumer (engine
// event loop) pops and returns them to the pool free list.
//
// The slot buffer is heap-allocated: Capacity=65536 × ~16 B ≈ 1 MiB per
// queue. Embedding std::array<slot, Capacity> in ObjectPool made engine
// ≈11 MiB and overflowed the default 8 MiB stack when engines were
// automatic-storage objects (tests, CLI, MC).
template<std::size_t Capacity = 65536>
class DeferredReturnQueue
{
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    DeferredReturnQueue()
        : slots_(std::make_unique<slot[]>(Capacity))
    {
    }

    bool try_push(void* item) noexcept
    {
        std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        for (;;)
        {
            const std::uint64_t head = head_.load(std::memory_order_acquire);
            if (tail - head >= Capacity)
                return false;

            if (tail_.compare_exchange_weak(tail, tail + 1,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed))
            {
                auto& s = slots_[tail & mask_];
                s.item = item;
                s.ready.store(tail + 1, std::memory_order_release);
                return true;
            }
        }
    }

    bool try_pop(void*& item) noexcept
    {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head >= tail)
            return false;

        auto& s = slots_[head & mask_];
        if (s.ready.load(std::memory_order_acquire) != head + 1)
            return false;

        item = s.item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    std::size_t pending() const noexcept
    {
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(tail - head);
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    struct slot
    {
        void* item = nullptr;
        std::atomic<std::uint64_t> ready{0};
    };

    static constexpr std::uint64_t mask_ = Capacity - 1;
    std::unique_ptr<slot[]> slots_;
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
};
