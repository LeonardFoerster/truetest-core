#pragma once

#include "threading/ring_buffer.h"
#include "types/public_trade.h"

#include <atomic>
#include <cstddef>

// Bounded SPSC ring carrying PublicTrade from the (single) DataBridge
// research tap producer to the (single) FootprintResearchService consumer.
//
// footprint.md §2.1: "Use one DataBridge producer and one
// FootprintResearchService consumer. Ring exhaustion will mark only
// research data as discontinuous and trigger recovery; it must never halt
// or slow the engine."
//
// Deliberately does NOT use RingBuffer's SpinWait/DropOldest/AssertFull
// push() wrapper: the producer must never block, spin, or throw on a full
// ring. try_push() always returns immediately; on overflow it drops the
// incoming trade (not an already-queued one) and marks the ring
// discontinuous, so the consumer sees a clean prefix instead of interior
// holes and can react in cold-path reconciliation (§2.2 RECOVERING).
namespace truetest::footprint {

template <std::size_t N>
class FootprintResearchRing
{
public:
    FootprintResearchRing() = default;
    FootprintResearchRing(const FootprintResearchRing&) = delete;
    FootprintResearchRing& operator=(const FootprintResearchRing&) = delete;

    // Producer side (research tap). Never allocates, locks, logs, retries,
    // or blocks. Returns false when the ring was full - the trade was
    // dropped and the ring is now marked discontinuous.
    bool try_push(const PublicTrade& trade) noexcept
    {
        if (ring_.try_push(trade))
            return true;

        discontinuity_count_.fetch_add(1, std::memory_order_relaxed);
        discontinuous_.store(true, std::memory_order_release);
        return false;
    }

    // Consumer side (FootprintResearchService). Single consumer only.
    bool try_pop(PublicTrade& out) noexcept
    {
        return ring_.try_pop(out);
    }

    // Sticky since the last acknowledge_discontinuity(); set as soon as any
    // push has been dropped.
    bool discontinuous() const noexcept
    {
        return discontinuous_.load(std::memory_order_acquire);
    }

    std::size_t discontinuity_count() const noexcept
    {
        return discontinuity_count_.load(std::memory_order_relaxed);
    }

    // Consumer-only: acknowledge the discontinuity once cold-path recovery
    // has been entered (RECOVERING, §2.2). Not safe to call from the
    // producer side - single-consumer ownership only.
    void acknowledge_discontinuity() noexcept
    {
        discontinuous_.store(false, std::memory_order_release);
    }

    std::size_t size() const noexcept { return ring_.size(); }
    static constexpr std::size_t capacity() noexcept { return N; }

private:
    // Policy template arg only affects RingBuffer::push(), which this class
    // never calls - try_push()/try_pop() are always non-blocking.
    RingBuffer<PublicTrade, N> ring_;
    std::atomic<bool> discontinuous_{false};
    std::atomic<std::size_t> discontinuity_count_{0};
};

} // namespace truetest::footprint
