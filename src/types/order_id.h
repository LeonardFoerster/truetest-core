#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

class OrderIdGenerator {
    struct local_sequence
    {
        bool active = false;
        std::uint64_t next = 1;
    };

    static std::atomic<uint64_t>& counter() {
        static std::atomic<uint64_t> c{1};
        return c;
    }

    static local_sequence& thread_sequence() noexcept
    {
        static thread_local local_sequence sequence;
        return sequence;
    }

public:
    class deterministic_scope final
    {
    public:
        explicit deterministic_scope(std::uint64_t start = 1) noexcept
        {
            auto& sequence = thread_sequence();
            previous_active_ = sequence.active;
            previous_next_ = sequence.next;
            sequence.active = true;
            sequence.next = start;
        }

        ~deterministic_scope()
        {
            auto& sequence = thread_sequence();
            sequence.active = previous_active_;
            sequence.next = previous_next_;
        }

        deterministic_scope(const deterministic_scope&) = delete;
        deterministic_scope& operator=(const deterministic_scope&) = delete;

    private:
        bool previous_active_{false};
        std::uint64_t previous_next_{1};
    };

    static uint64_t next() noexcept {
        auto& sequence = thread_sequence();
        if (sequence.active)
        {
            const std::uint64_t value = sequence.next;
            sequence.next = value == std::numeric_limits<std::uint64_t>::max()
                ? 0 : value + 1;
            return value;
        }
        return counter().fetch_add(1, std::memory_order_relaxed);
    }

    static void reset(uint64_t start = 1) noexcept {
        auto& sequence = thread_sequence();
        if (sequence.active)
        {
            sequence.next = start;
            return;
        }
        counter().store(start, std::memory_order_relaxed);
    }
};
