#pragma once

#include "types/pool_exhausted.h"
#include "types/pool_free_stack.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

template<typename T>
class control_block_allocator;

// Fixed-size slots for std::shared_ptr control blocks (Phase 2 hot-path).
// libstdc++ counted_ptr + deleter typically fits in 48–64 bytes for our
// event types; rebinding guards below fall back to std::allocator for any
// implementation-specific control block that does not fit this contract.
class ControlBlockPool
{
    struct node : pool_free_node
    {};

    static constexpr std::size_t SlotSize = 64;
    static constexpr std::size_t SlotAlignment = 64;
    static constexpr std::size_t BlockSize = 4096;
    static_assert(SlotSize >= sizeof(node));

    struct alignas(SlotAlignment) block
    {
        alignas(SlotAlignment) unsigned char storage[SlotSize * BlockSize];
    };

    // The facade is intentionally separate from the backing state. Rebound
    // allocators stored in shared_ptr control blocks retain State, so the
    // control-block bytes outlive an engine/facade that has already been
    // destroyed.
    struct State
    {
        mutable std::mutex grow_mutex;
        std::vector<std::unique_ptr<block>> blocks;

        PoolFreeStack free_stack;
        PoolSingleConsumerGate pop_gate;

        std::atomic<std::size_t> block_count_atomic{0};
        std::atomic<std::size_t> in_use_atomic{0};
        std::atomic<std::size_t> grow_count_atomic{0};
        std::atomic<std::size_t> fallback_allocations_atomic{0};

        bool forbid_runtime_grow = false;
        const char* pool_name = "control_block_pool";

        State() { grow(false); }

        void push_nodes_to_stack(node* head) noexcept
        {
            while (head)
            {
                node* next = static_cast<node*>(head->next);
                free_stack.push(head);
                head = next;
            }
        }

        void grow(bool count_runtime_grow)
        {
            auto blk = std::make_unique<block>();
            unsigned char* base = blk->storage;

            node* batch_head = nullptr;
            for (std::size_t i = 0; i < BlockSize; ++i)
            {
                auto* n = std::construct_at(
                    reinterpret_cast<node*>(base + i * SlotSize));
                n->next = batch_head;
                batch_head = n;
            }

            blocks.push_back(std::move(blk));
            block_count_atomic.store(blocks.size(), std::memory_order_release);
            push_nodes_to_stack(batch_head);
            if (count_runtime_grow)
                grow_count_atomic.fetch_add(1, std::memory_order_relaxed);
        }

        void* pop()
        {
            std::lock_guard<PoolSingleConsumerGate> consumer_lock(pop_gate);

            if (auto* n = static_cast<node*>(free_stack.pop()))
                return static_cast<void*>(n);

            std::lock_guard<std::mutex> lock(grow_mutex);
            if (free_stack.empty())
            {
                if (forbid_runtime_grow)
                    throw pool_exhausted(pool_name);
                grow(true);
            }

            if (auto* n = static_cast<node*>(free_stack.pop()))
                return static_cast<void*>(n);

            throw pool_exhausted(pool_name);
        }

        void acquire_slot(void*& slot)
        {
            slot = pop();
            in_use_atomic.fetch_add(1, std::memory_order_relaxed);
        }

        void release_slot(void* ptr) noexcept
        {
            if (!ptr)
                return;
            // The shared_ptr control block (or direct raw-slot user) ended the
            // prior object lifetime before returning this storage.
            free_stack.push(std::construct_at(static_cast<node*>(ptr)));
            in_use_atomic.fetch_sub(1, std::memory_order_relaxed);
        }

#ifdef HAS_DEBUG
        std::size_t debug_footprint_bytes() const
        {
            std::lock_guard<std::mutex> lock(grow_mutex);
            return blocks.size() * BlockSize * SlotSize;
        }
#endif
    };

    std::shared_ptr<State> state_ = std::make_shared<State>();

    template<typename T>
    friend class control_block_allocator;

public:
    ControlBlockPool() = default;

    ControlBlockPool(const ControlBlockPool&) = delete;
    ControlBlockPool& operator=(const ControlBlockPool&) = delete;

    static constexpr std::size_t slot_size() noexcept { return SlotSize; }
    static constexpr std::size_t slot_alignment() noexcept { return SlotAlignment; }
    static constexpr std::size_t slots_per_block() noexcept { return BlockSize; }

    void drain_deferred_returns() noexcept
    {
        // Returns are pushed directly into the MPSC free stack. Kept as an
        // API-compatible no-op for existing engine cleanup call sites.
    }

    std::size_t deferred_pending() const noexcept
    {
        return 0;
    }

    std::size_t deferred_overflow() const noexcept
    {
        return 0;
    }

    std::size_t fallback_allocations() const noexcept
    {
        return state_->fallback_allocations_atomic.load(
            std::memory_order_relaxed);
    }

    void* acquire_slot()
    {
        void* slot = nullptr;
        state_->acquire_slot(slot);
        return slot;
    }

    void release_slot(void* ptr) noexcept
    {
        state_->release_slot(ptr);
    }

    std::size_t in_use() const noexcept
    {
        return state_->in_use_atomic.load(std::memory_order_relaxed);
    }

    std::size_t grow_count() const noexcept
    {
        return state_->grow_count_atomic.load(std::memory_order_relaxed);
    }

    std::size_t block_count() const noexcept
    {
        return state_->block_count_atomic.load(std::memory_order_acquire);
    }

    std::size_t capacity_slots() const noexcept
    {
        return block_count() * BlockSize;
    }

    void set_pool_name(const char* name) noexcept
    {
        state_->pool_name = (name && name[0]) ? name : "control_block_pool";
    }

    void set_forbid_runtime_grow(bool forbid) noexcept
    {
        state_->forbid_runtime_grow = forbid;
    }

    void ensure_min_blocks(std::size_t min_blocks)
    {
        std::lock_guard<std::mutex> lock(state_->grow_mutex);
        while (state_->blocks.size() < min_blocks)
            state_->grow(false);
    }

#ifdef HAS_DEBUG
    std::size_t debug_footprint_bytes() const
    {
        return state_->debug_footprint_bytes();
    }
#endif
};

// STL allocator facade; std::shared_ptr(ptr, deleter, alloc) uses this for
// the control block only. It owns the backing State, never a facade pointer.
template<typename T>
class control_block_allocator
{
    static constexpr bool FitsPoolSlot =
        sizeof(T) <= ControlBlockPool::slot_size() &&
        alignof(T) <= ControlBlockPool::slot_alignment();

public:
    using value_type = T;

    template<typename U>
    struct rebind
    {
        using other = control_block_allocator<U>;
    };

    control_block_allocator() noexcept = default;

    explicit control_block_allocator(ControlBlockPool* pool) noexcept
        : state_(pool ? pool->state_ : nullptr)
    {}

    template<typename U>
    explicit control_block_allocator(const control_block_allocator<U>& other) noexcept
        : state_(other.state_)
    {}

    T* allocate(std::size_t n)
    {
        if constexpr (FitsPoolSlot)
        {
            if (state_ && n == 1)
            {
                void* slot = nullptr;
                state_->acquire_slot(slot);
                return static_cast<T*>(slot);
            }
        }

        if (state_ && state_->forbid_runtime_grow)
            throw pool_exhausted("control_block_pool_fallback");

        T* result = std::allocator<T>{}.allocate(n);
        if (state_)
            state_->fallback_allocations_atomic.fetch_add(
                1, std::memory_order_relaxed);
        return result;
    }

    void deallocate(T* p, std::size_t n) noexcept
    {
        if (!p)
            return;

        if constexpr (FitsPoolSlot)
        {
            if (state_ && n == 1)
            {
                state_->release_slot(p);
                return;
            }
        }

        std::allocator<T>{}.deallocate(p, n);
    }

    bool has_pool() const noexcept { return static_cast<bool>(state_); }
    const void* state_identity() const noexcept { return state_.get(); }

    template<typename U>
    friend class control_block_allocator;

private:
    std::shared_ptr<typename ControlBlockPool::State> state_;
};

template<typename T, typename U>
bool operator==(const control_block_allocator<T>& a,
                const control_block_allocator<U>& b) noexcept
{
    return a.state_identity() == b.state_identity();
}

template<typename T, typename U>
bool operator!=(const control_block_allocator<T>& a,
                const control_block_allocator<U>& b) noexcept
{
    return !(a == b);
}
