#pragma once

#include "types/control_block_pool.h"
#include "types/deferred_return_queue.h"
#include "types/pool_exhausted.h"
#include "types/pool_free_stack.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>
#include <new>

template<typename T, std::size_t BlockSize = 4096>
class ObjectPool
{
    struct node : pool_free_node
    {};

    static constexpr std::size_t SlotSize =
        sizeof(T) >= sizeof(node) ? sizeof(T) : sizeof(node);
    static constexpr std::size_t SlotAlign =
        alignof(T) >= alignof(node) ? alignof(T) : alignof(node);

    struct alignas(SlotAlign) block
    {
        alignas(SlotAlign) unsigned char storage[SlotSize * BlockSize];
    };

    // grow_mutex_ protects blocks_ and runtime grow() only (cold path).
    mutable std::mutex grow_mutex_;
    std::vector<std::unique_ptr<block>> blocks_;

    PoolFreeStack free_stack_;
    DeferredReturnQueue<> deferred_returns_;

    std::atomic<std::size_t> block_count_atomic_{0};
    std::atomic<std::size_t> in_use_atomic_{0};
    std::atomic<std::size_t> grow_count_atomic_{0};
    std::atomic<std::size_t> deferred_overflow_atomic_{0};

    bool forbid_runtime_grow_ = false;
    const char* pool_name_ = "ObjectPool";
    ControlBlockPool* control_block_pool_ = nullptr;

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

    ObjectPool()
    {
        grow(false);
    }

    ~ObjectPool() = default;

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    void set_control_block_pool(ControlBlockPool* pool) noexcept
    {
        control_block_pool_ = pool;
    }

    template<typename... Args>
    std::shared_ptr<T> acquire(Args&&... args)
    {
        void* slot = pop();
        in_use_atomic_.fetch_add(1, std::memory_order_relaxed);
        T* obj = new (slot) T(std::forward<Args>(args)...);

        auto deleter = [this](T* p) {
            p->~T();
            push(static_cast<void*>(p));
            in_use_atomic_.fetch_sub(1, std::memory_order_relaxed);
        };

        if (control_block_pool_)
        {
            control_block_allocator<std::byte> alloc(control_block_pool_);
            return std::shared_ptr<T>(obj, std::move(deleter), alloc);
        }

        return std::shared_ptr<T>(obj, std::move(deleter));
    }

    std::size_t in_use() const
    {
        return in_use_atomic_.load(std::memory_order_relaxed);
    }

    std::size_t grow_count() const
    {
        return grow_count_atomic_.load(std::memory_order_relaxed);
    }

    static constexpr std::size_t slots_per_block() { return BlockSize; }

    std::size_t capacity_slots() const
    {
        return block_count() * BlockSize;
    }

    void set_pool_name(const char* name) noexcept
    {
        pool_name_ = (name && name[0]) ? name : "ObjectPool";
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

    std::size_t block_count() const
    {
        return block_count_atomic_.load(std::memory_order_acquire);
    }

    std::size_t block_count_locked() const
    {
        std::lock_guard<std::mutex> lock(grow_mutex_);
        return blocks_.size();
    }

#ifdef HAS_DEBUG
    std::size_t debug_footprint_bytes() const
    {
        std::lock_guard<std::mutex> lock(grow_mutex_);
        return blocks_.size() * BlockSize * SlotSize;
    }
#endif
};