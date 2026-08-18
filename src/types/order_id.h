#pragma once
#include <atomic>
#include <cstdint>

class OrderIdGenerator {
    static std::atomic<uint64_t>& counter() {
        static std::atomic<uint64_t> c{1};
        return c;
    }
public:
    static uint64_t next() {
        return counter().fetch_add(1, std::memory_order_relaxed);
    }

    // Recovery reserves every durable opener identity before the engine can
    // create a new local order or synthesize a native-bracket closer.  This
    // operation is monotonic: a concurrent caller may advance farther, but a
    // stale recovery scan can never move the generator backwards and reuse a
    // live/recovered identity.
    static bool advance_to_at_least(uint64_t next_id) noexcept {
        if (next_id == 0) return false;  // wrapped/invalid reservation target
        auto observed = counter().load(std::memory_order_relaxed);
        while (observed < next_id) {
            if (counter().compare_exchange_weak(
                    observed, next_id,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed))
                return true;
        }
        return true;
    }

    static void reset(uint64_t start = 1) {
        counter().store(start, std::memory_order_relaxed);
    }
};
