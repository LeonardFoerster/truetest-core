#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Lock-free Treiber stack for pool free slots. Returners may push from many
// threads; exactly one pop operation is serialized by PoolSingleConsumerGate.
struct pool_free_node
{
    pool_free_node* next = nullptr;
};

class PoolSingleConsumerGate
{
public:
    void lock() noexcept
    {
        while (locked_.test_and_set(std::memory_order_acquire))
            locked_.wait(true, std::memory_order_relaxed);
    }

    void unlock() noexcept
    {
        locked_.clear(std::memory_order_release);
        locked_.notify_one();
    }

private:
    std::atomic_flag locked_ = ATOMIC_FLAG_INIT;
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
                                            std::memory_order_acquire))
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
