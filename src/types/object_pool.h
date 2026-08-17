#pragma once

#include "types/control_block_pool.h"
#include "types/pool_exhausted.h"
#include "types/pool_free_stack.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

template<typename T, std::size_t BlockSize = 4096>
class ObjectPool
{
    struct node : pool_free_node {};

    static constexpr std::size_t RawSlotSize =
        sizeof(T) >= sizeof(node) ? sizeof(T) : sizeof(node);
    static constexpr std::size_t SlotAlign =
        alignof(T) >= alignof(node) ? alignof(T) : alignof(node);
    static constexpr std::size_t SlotSize =
        ((RawSlotSize + SlotAlign - 1) / SlotAlign) * SlotAlign;
    static_assert(SlotSize % SlotAlign == 0);

    struct alignas(SlotAlign) block
    {
        alignas(SlotAlign) unsigned char storage[SlotSize * BlockSize];
    };

    // State owns all storage and synchronization used by Returner. A pooled
    // shared_ptr may outlive the ObjectPool facade, so Returner retains this
    // state rather than a raw ObjectPool pointer or an advisory lifetime bit.
    struct State
    {
        mutable std::mutex grow_mutex;
        PoolFreeStack free_stack;
        PoolSingleConsumerGate pop_gate;
        std::vector<std::unique_ptr<block>> blocks;

        // Lock-free shadow of blocks.size() for cheap reads from the dashboard
        // snapshot path. Updated under mutex in grow(); read without locking
        // via block_count().
        std::atomic<std::size_t> block_count_atomic{0};
        std::atomic<std::size_t> in_use_atomic{0};
        std::atomic<std::size_t> grow_count_atomic{0};

        bool forbid_runtime_grow = false;
        const char* pool_name = "object_pool";

        State() { grow(false); }

        void push_nodes_to_stack(node* head) noexcept
        {
            while (head)
            {
                auto* next = static_cast<node*>(head->next);
                free_stack.push(head);
                head = next;
            }
        }

        void grow(bool count_runtime_grow = true)
        {
            auto blk = std::make_unique<block>();
            // vector growth may allocate. Publish the block before linking any
            // of its nodes into the free stack, otherwise a bad_alloc would
            // leave it pointing into the local block that is about to die.
            blocks.push_back(std::move(blk));
            unsigned char* base = blocks.back()->storage;

            node* batch_head = nullptr;
            for (std::size_t i = 0; i < BlockSize; ++i)
            {
                auto* n = std::construct_at(
                    reinterpret_cast<node*>(base + i * SlotSize));
                n->next = batch_head;
                batch_head = n;
            }

            push_nodes_to_stack(batch_head);
            block_count_atomic.store(blocks.size(), std::memory_order_release);
            if (count_runtime_grow)
                grow_count_atomic.fetch_add(1, std::memory_order_relaxed);
        }

        void* pop()
        {
            std::lock_guard<PoolSingleConsumerGate> consumer_lock(pop_gate);
            if (auto* n = static_cast<node*>(free_stack.pop()))
                return static_cast<void*>(n);

            std::lock_guard<std::mutex> lock(grow_mutex);
            if (auto* n = static_cast<node*>(free_stack.pop()))
                return static_cast<void*>(n);
            if (forbid_runtime_grow)
                throw pool_exhausted(pool_name ? std::string(pool_name) : "object_pool");
            grow(true);

            if (auto* n = static_cast<node*>(free_stack.pop()))
                return static_cast<void*>(n);
            throw pool_exhausted(pool_name ? std::string(pool_name) : "object_pool");
        }

        void push(void* ptr)
        {
            // Placement construction of T ended the previous node lifetime.
            // Restart it before using the slot as intrusive free-list storage.
            auto* n = std::construct_at(static_cast<node*>(ptr));
            free_stack.push(n);
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

    // Kept outside State so ObjectPool::Returner only retains object storage;
    // a shared_ptr control block independently retains its ControlBlockPool
    // state through the rebound allocator stored in that control block.
    control_block_allocator<char> cb_allocator_;

public:
    ObjectPool() = default;
    ~ObjectPool() = default;

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template<typename... Args>
    std::shared_ptr<T> acquire(Args&&... args)
    {
        auto state = state_;
        void* slot = state->pop();

        T* obj = nullptr;
        try
        {
            obj = new (slot) T(std::forward<Args>(args)...);
        }
        catch (...)
        {
            state->push(slot);
            throw;
        }
        state->in_use_atomic.fetch_add(1, std::memory_order_relaxed);

        struct Returner
        {
            // mutable lets the State lease end at the strong-object lifetime,
            // rather than unnecessarily pinning an ObjectPool State through a
            // long-lived weak_ptr after the object has been destroyed.
            mutable std::shared_ptr<State> state;

            void operator()(T* p) const noexcept
            {
                auto owned_state = std::move(state);
                p->~T();
                owned_state->push(static_cast<void*>(p));
                owned_state->in_use_atomic.fetch_sub(1, std::memory_order_relaxed);
            }
        };
        Returner dtor_and_return{std::move(state)};

        if (cb_allocator_.has_pool())
            return std::shared_ptr<T>(obj, std::move(dtor_and_return), cb_allocator_);

        return std::shared_ptr<T>(obj, std::move(dtor_and_return));
    }

    std::size_t in_use() const noexcept
    {
        return state_->in_use_atomic.load(std::memory_order_relaxed);
    }

    // Lock-free read of the current block count. Safe to call from any
    // thread; consistency is eventually correct. Callers needing strict
    // consistency should use block_count_locked().
    std::size_t block_count() const noexcept
    {
        return state_->block_count_atomic.load(std::memory_order_acquire);
    }

    std::size_t block_count_locked() const
    {
        std::lock_guard<std::mutex> lock(state_->grow_mutex);
        return state_->blocks.size();
    }

    void set_pool_name(const char* name) noexcept
    {
        state_->pool_name = (name && name[0]) ? name : "object_pool";
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

    std::size_t capacity_slots() const noexcept
    {
        return block_count() * BlockSize;
    }

    std::size_t grow_count() const noexcept
    {
        return state_->grow_count_atomic.load(std::memory_order_relaxed);
    }

    std::size_t deferred_pending() const noexcept
    {
        return 0;
    }

    void drain_deferred_returns() noexcept
    {
        // Returns are pushed directly into the MPSC free stack. Kept as an
        // API-compatible no-op for existing engine cleanup call sites.
    }

    void set_control_block_pool(ControlBlockPool* pool) noexcept
    {
        cb_allocator_ = control_block_allocator<char>(pool);
    }

    // The engine calls this seam between reusable Monte Carlo trials. State
    // ownership makes old references safe: they keep their slots reserved and
    // return normally when released, rather than being leaked or discarded.
    void rearm_for_reuse() noexcept {}

#ifdef HAS_DEBUG
    std::size_t debug_footprint_bytes() const
    {
        return state_->debug_footprint_bytes();
    }
#endif
};
