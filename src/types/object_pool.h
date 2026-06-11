#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>
#include <new>

template<typename T, std::size_t BlockSize = 4096>
class ObjectPool
{
    struct node
    {
        node* next;
    };

    static constexpr std::size_t SlotSize =
        sizeof(T) >= sizeof(node) ? sizeof(T) : sizeof(node);
    static constexpr std::size_t SlotAlign =
        alignof(T) >= alignof(node) ? alignof(T) : alignof(node);

    struct alignas(SlotAlign) block
    {
        alignas(SlotAlign) unsigned char storage[SlotSize * BlockSize];
    };

    mutable std::mutex mutex_;
    node* free_head_ = nullptr;
    std::vector<std::unique_ptr<block>> blocks_;

    // Lock-free shadow of blocks_.size() for cheap reads from the
    // dashboard snapshot path. Updated under mutex_ in grow(); read
    // without locking via block_count(). block_count_locked() is kept
    // for callers that want a strict consistency guarantee.
    std::atomic<std::size_t> block_count_atomic_{0};

    // Live in-use count for the dashboard's "fill" bar. Bumped on
    // acquire, decremented in the shared_ptr deleter. Relaxed ordering
    // - observers only need eventual consistency, and pop/push already
    // serialize through the mutex.
    std::atomic<std::size_t> in_use_atomic_{0};

    void grow()
    {
        auto blk = std::make_unique<block>();
        unsigned char* base = blk->storage;

        for (std::size_t i = 0; i < BlockSize; ++i)
        {
            auto* n = reinterpret_cast<node*>(base + i * SlotSize);
            n->next = free_head_;
            free_head_ = n;
        }

        blocks_.push_back(std::move(blk));
        block_count_atomic_.store(blocks_.size(), std::memory_order_release);
    }

    void* pop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!free_head_)
            grow();

        node* n = free_head_;
        free_head_ = n->next;
        return static_cast<void*>(n);
    }

    void push(void* ptr)
    {
        auto* n = static_cast<node*>(ptr);
        std::lock_guard<std::mutex> lock(mutex_);
        n->next = free_head_;
        free_head_ = n;
    }

public:
    ObjectPool()
    {
        grow();
    }

    ~ObjectPool() = default;

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template<typename... Args>
    std::shared_ptr<T> acquire(Args&&... args)
    {
        void* slot = pop();
        in_use_atomic_.fetch_add(1, std::memory_order_relaxed);
        T* obj = new (slot) T(std::forward<Args>(args)...);
        return std::shared_ptr<T>(obj, [this](T* p) {
            p->~T();
            push(static_cast<void*>(p));
            in_use_atomic_.fetch_sub(1, std::memory_order_relaxed);
        });
    }

    std::size_t in_use() const
    {
        return in_use_atomic_.load(std::memory_order_relaxed);
    }

    // Lock-free read of the current block count. Safe to call from any
    // thread; consistency is "eventually correct" - a grow on another
    // thread may have happened between the load here and downstream use.
    // For dashboard snapshots that's strictly fine (the cache is rebuilt
    // every 1s anyway). Callers needing strict consistency should use
    // block_count_locked() instead.
    std::size_t block_count() const
    {
        return block_count_atomic_.load(std::memory_order_acquire);
    }

    std::size_t block_count_locked() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return blocks_.size();
    }

#ifdef HAS_DEBUG
    std::size_t debug_footprint_bytes() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return blocks_.size() * BlockSize * SlotSize;
    }
#endif
};
