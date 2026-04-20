#pragma once

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
        grow();
    }

    ~ObjectPool() = default;

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

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
