#pragma once
#include <atomic>
#include <cstdint>

// Thread-safe monotonic order ID generator.
// Single global instance — all components draw from the same counter.
class OrderIdGenerator {
    static std::atomic<uint64_t>& counter() {
        static std::atomic<uint64_t> c{1};
        return c;
    }
public:
    static uint64_t next() {
        return counter().fetch_add(1, std::memory_order_relaxed);
    }

    // Reset counter (for testing only)
    static void reset(uint64_t start = 1) {
        counter().store(start, std::memory_order_relaxed);
    }
};
