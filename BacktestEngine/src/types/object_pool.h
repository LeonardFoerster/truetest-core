#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>
#include <new>

// Thread-safe object pool with block-based pre-allocation.
// Objects are returned to the pool via custom shared_ptr deleter.
// Uses a spinlock-protected freelist. The performance win is avoiding
// malloc/free on the hot path, not lock elimination — the critical
// section is ~3 pointer ops, so contention is negligible.
template<typename T, std::size_t BlockSize = 4096>
class ObjectPool
{
    // Freelist node — overlaid on unused slots when not in use
    struct node
    {
        node* next;
    };

    // Slot size: at least large enough for a freelist pointer
    static constexpr std::size_t SlotSize =
        sizeof(T) >= sizeof(node) ? sizeof(T) : sizeof(node);
    static constexpr std::size_t SlotAlign =
        alignof(T) >= alignof(node) ? alignof(T) : alignof(node);

    // Raw storage block, kept alive for pool lifetime
    struct alignas(SlotAlign) block
    {
        alignas(SlotAlign) unsigned char storage[SlotSize * BlockSize];
    };

    mutable std::mutex mutex_;
    node* free_head_ = nullptr;
    std::vector<std::unique_ptr<block>> blocks_;

    // Allocate a new block and push all slots onto the freelist.
    // Caller must hold mutex_.
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
        grow(); // Pre-allocate first block
    }

    ~ObjectPool() = default;

    // Non-copyable, non-movable (pointers into blocks must remain stable)
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // Acquire a shared_ptr<T> from the pool, constructing T with the given args.
    // When the shared_ptr refcount reaches zero, the object is destroyed and
    // the memory is returned to the pool — no heap deallocation occurs.
    template<typename... Args>
    std::shared_ptr<T> acquire(Args&&... args)
    {
        void* slot = pop();
        T* obj = new (slot) T(std::forward<Args>(args)...);
        return std::shared_ptr<T>(obj, [this](T* p) {
            p->~T();
            push(static_cast<void*>(p));
        });
    }

    // Number of blocks allocated (for testing)
    std::size_t block_count() const
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
