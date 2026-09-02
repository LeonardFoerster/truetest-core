#pragma once

#include "providers/footprint/footprint_ring.h"
#include "types/footprint_data_status.h"
#include "types/public_trade.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Cold-path consumer of the footprint research ring. footprint.md §2.1:
// "Use one DataBridge producer and one FootprintResearchService consumer."
//
// Phase 1 scope only: drains the ring into a small bounded working set and
// exposes read-only counters/status. Bar aggregation, POC/CVD, cache
// persistence, and full reconciliation are §2.2 (a later phase) - this
// class is deliberately not that yet; it is the ingress seam those phases
// attach to.
//
// Threading: drain_once() must be called from exactly one thread (the
// consumer side of the SPSC ring - typically a dedicated cold worker, never
// the engine event loop). Read-only accessors may be called from any
// thread). received_count() and discontinuity_events() are atomic diagnostic
// counters and may be read from any thread. status(), set_status(), and all
// working-set accessors are consumer-thread-only; returning references to a
// concurrently-mutated working set would otherwise be a data race.
namespace truetest::footprint {

template <std::size_t RingN, std::size_t WorkingSetN = 4096>
class FootprintResearchService
{
    static_assert((WorkingSetN & (WorkingSetN - 1)) == 0,
                  "working set capacity must be a power of 2");

public:
    explicit FootprintResearchService(FootprintResearchRing<RingN>& ring)
        : ring_(ring)
    {}

    FootprintResearchService(const FootprintResearchService&) = delete;
    FootprintResearchService& operator=(const FootprintResearchService&) = delete;

    // Drains up to max_items from the ring into the bounded working set.
    // Returns the number of trades drained. Never blocks - stops as soon as
    // the ring is empty or max_items is reached, so a caller on a periodic
    // cold-worker tick always returns promptly.
    std::size_t drain_once(std::size_t max_items = RingN)
    {
        std::size_t drained = 0;
        PublicTrade t;
        while (drained < max_items && ring_.try_pop(t))
        {
            working_set_[write_pos_ & kMask] = t;
            ++write_pos_;
            if (count_ < WorkingSetN)
                ++count_;
            ++drained;
        }
        if (drained > 0)
            received_count_.fetch_add(drained, std::memory_order_relaxed);

        const std::size_t discontinuity_generation =
            ring_.discontinuity_generation();
        if (ring_.discontinuous())
        {
            status_ = data_status::recovering;
            ring_.acknowledge_discontinuity(discontinuity_generation);
            discontinuity_events_.fetch_add(1, std::memory_order_relaxed);
        }
        return drained;
    }

    std::size_t received_count() const noexcept
    {
        return received_count_.load(std::memory_order_relaxed);
    }

    std::size_t discontinuity_events() const noexcept
    {
        return discontinuity_events_.load(std::memory_order_relaxed);
    }

    data_status status() const noexcept { return status_; }

    // Explicit status transitions - §2.2 owns the real reconciliation state
    // machine; this is storage that phase will drive. Exposed now so tests
    // and the desk (§2.3) status chip have something honest to read.
    void set_status(data_status s) noexcept { status_ = s; }

    std::size_t working_set_size() const noexcept { return count_; }

    // logical_index 0 == oldest still-retained trade, working_set_size()-1
    // == most recently drained. Caller must keep logical_index < working_set_size().
    const PublicTrade& working_set_at(std::size_t logical_index) const
    {
        const std::size_t start = write_pos_ - count_;
        return working_set_[(start + logical_index) & kMask];
    }

private:
    static constexpr std::size_t kMask = WorkingSetN - 1;

    FootprintResearchRing<RingN>& ring_;
    std::array<PublicTrade, WorkingSetN> working_set_{};
    std::size_t write_pos_ = 0;
    std::size_t count_ = 0;

    std::atomic<std::size_t> received_count_{0};
    std::atomic<std::size_t> discontinuity_events_{0};
    data_status status_ = data_status::unavailable;
};

} // namespace truetest::footprint
