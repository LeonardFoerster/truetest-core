#pragma once

#include "types/deferred_return_queue.h"
#include "types/pool_exhausted.h"
#include "types/pool_free_stack.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

// Fixed-size slots for std::shared_ptr control blocks (Phase 2 hot-path).
// libstdc++ counted_ptr + deleter typically fits in 48–64 bytes for our
// event types; 64 bytes is the conservative slot size.
class ControlBlockPool
{
    struct node : pool_free_node
    {};

    static constexpr std::size_t SlotSize = 64;
    static constexpr std::size_t BlockSize = 4096;
    static_assert(SlotSize >= sizeof(node));

    struct alignas(64) block
    {
        alignas(64) unsigned char storage[SlotSize * BlockSize];
    };

    mutable std::mutex grow_mutex_;
    std::vector<std::unique_ptr<block>> blocks_;

    PoolFreeStack free_stack_;
    DeferredReturnQueue<> deferred_returns_;

    std::atomic<std::size_t> block_count_atomic_{0};
    std::atomic<std::size_t> in_use_atomic_{0};
    std::atomic<std::size_t> grow_count_atomic_{0};
    std::atomic<std::size_t> deferred_overflow_atomic_{0};

    bool forbid_runtime_grow_ = false;
    const char* pool_name_ = "control_block_pool";

    void push_nodes_to_stack(node* head)
    {
        while (head)
        {
            node* next = static_cast<node*>(head->next);
            free_stack_.push(head);
            head = next;
        }
    }

    void grow(bool count_runtime_grow)
    {
        if (count_runtime_grow)
            grow_count_atomic_.fetch_add(1, std::memory_order_relaxed);

        auto blk = std::make_unique<block>();
        unsigned char* base = blk->storage;

        node* batch_head = nullptr;
        for (std::size_t i = 0; i < BlockSize; ++i)
        {
            auto* n = reinterpret_cast<node*>(base + i * SlotSize);
            n->next = batch_head;
            batch_head = n;
        }

        blocks_.push_back(std::move(blk));
        block_count_atomic_.store(blocks_.size(), std::memory_order_release);
        push_nodes_to_stack(batch_head);
    }

    void defer_release(void* ptr) noexcept
    {
        if (!deferred_returns_.try_push(ptr))
        {
            deferred_overflow_atomic_.fetch_add(1, std::memory_order_relaxed);
            free_stack_.push(static_cast<node*>(ptr));
        }
    }

    void* pop()
    {
        drain_deferred_returns();

        if (auto* n = static_cast<node*>(free_stack_.pop()))
            return static_cast<void*>(n);

        std::lock_guard<std::mutex> lock(grow_mutex_);
        if (free_stack_.empty())
        {
            if (forbid_runtime_grow_)
                throw pool_exhausted(pool_name_);
            grow(true);
        }

        if (auto* n = static_cast<node*>(free_stack_.pop()))
            return static_cast<void*>(n);

        throw pool_exhausted(pool_name_);
    }

    void push(void* ptr) noexcept
    {
        defer_release(ptr);
    }

public:
    void drain_deferred_returns() noexcept
    {
        void* ptr = nullptr;
        while (deferred_returns_.try_pop(ptr))
            free_stack_.push(static_cast<node*>(ptr));
    }

    std::size_t deferred_pending() const noexcept
    {
        return deferred_returns_.pending();
    }

    std::size_t deferred_overflow() const noexcept
    {
        return deferred_overflow_atomic_.load(std::memory_order_relaxed);
    }

    ControlBlockPool() { grow(false); }

    ControlBlockPool(const ControlBlockPool&) = delete;
    ControlBlockPool& operator=(const ControlBlockPool&) = delete;

    static constexpr std::size_t slot_size() { return SlotSize; }
    static constexpr std::size_t slots_per_block() { return BlockSize; }

    void* acquire_slot()
    {
        void* slot = pop();
        in_use_atomic_.fetch_add(1, std::memory_order_relaxed);
        return slot;
    }

    void release_slot(void* ptr) noexcept
    {
        if (!ptr) return;
        push(ptr);
        in_use_atomic_.fetch_sub(1, std::memory_order_relaxed);
    }

    std::size_t in_use() const
    {
        return in_use_atomic_.load(std::memory_order_relaxed);
    }

    std::size_t grow_count() const
    {
        return grow_count_atomic_.load(std::memory_order_relaxed);
    }

    std::size_t block_count() const
    {
        return block_count_atomic_.load(std::memory_order_acquire);
    }

    std::size_t capacity_slots() const
    {
        return block_count() * BlockSize;
    }

    void set_pool_name(const char* name) noexcept
    {
        pool_name_ = (name && name[0]) ? name : "control_block_pool";
    }

    void set_forbid_runtime_grow(bool forbid) noexcept
    {
        forbid_runtime_grow_ = forbid;
    }

    void ensure_min_blocks(std::size_t min_blocks)
    {
        std::lock_guard<std::mutex> lock(grow_mutex_);
        while (blocks_.size() < min_blocks)
            grow(false);
    }

#ifdef HAS_DEBUG
    std::size_t debug_footprint_bytes() const
    {
        std::lock_guard<std::mutex> lock(grow_mutex_);
        return blocks_.size() * BlockSize * SlotSize;
    }
#endif
};

// STL allocator facade; std::shared_ptr(ptr, deleter, alloc) uses this for
// the control block only (C++20).
template<typename T>
class control_block_allocator
{
public:
    using value_type = T;

    template<typename U>
    struct rebind
    {
        using other = control_block_allocator<U>;
    };

    control_block_allocator() noexcept : pool_(nullptr) {}

    explicit control_block_allocator(ControlBlockPool* pool) noexcept
        : pool_(pool)
    {}

    template<typename U>
    explicit control_block_allocator(const control_block_allocator<U>& other) noexcept
        : pool_(other.pool())
    {}

    T* allocate(std::size_t n)
    {
        if (!pool_ || n != 1)
            throw std::bad_alloc();
        return static_cast<T*>(pool_->acquire_slot());
    }

    void deallocate(T* p, std::size_t n) noexcept
    {
        if (pool_ && p && n == 1)
            pool_->release_slot(p);
    }

    ControlBlockPool* pool() const noexcept { return pool_; }

    template<typename U>
    friend class control_block_allocator;

private:
    ControlBlockPool* pool_;
};

template<typename T, typename U>
bool operator==(const control_block_allocator<T>& a,
                const control_block_allocator<U>& b) noexcept
{
    return a.pool() == b.pool();
}

template<typename T, typename U>
bool operator!=(const control_block_allocator<T>& a,
                const control_block_allocator<U>& b) noexcept
{
    return !(a == b);
}