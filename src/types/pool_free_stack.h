#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Phase 3: lock-free Treiber stack for pool free slots. Single consumer
// (engine acquire path) pops; producers push after draining deferred returns.
struct pool_free_node
{
    pool_free_node* next = nullptr;
};

class PoolFreeStack
{
public:
    void push(pool_free_node* node) noexcept
    {
        pool_free_node* old = head_.load(std::memory_order_relaxed);
        for (;;)
        {
            node->next = old;
            if (head_.compare_exchange_weak(old, node,
                                            std::memory_order_release,
                                            std::memory_order_relaxed))
                return;
        }
    }

    pool_free_node* pop() noexcept
    {
        pool_free_node* old = head_.load(std::memory_order_acquire);
        for (;;)
        {
            if (!old)
                return nullptr;

            pool_free_node* next = old->next;
            if (head_.compare_exchange_weak(old, next,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed))
                return old;
        }
    }

    bool empty() const noexcept
    {
        return head_.load(std::memory_order_acquire) == nullptr;
    }

private:
    std::atomic<pool_free_node*> head_{nullptr};
};