#pragma once

#include "types/public_trade.h"

#include <cstdint>
#include <map>
#include <vector>

// footprint.md §2.2: "order trades through the two-second reorder window."
// Buffers trades briefly so minor out-of-order network arrival gets
// re-sorted by event_ns before reaching the aggregator; a trade arriving
// even later than the window has already closed for is rejected outright
// by offer() - footprint.md's "an arrival older than the reorder window"
// fault trigger. Rejection is reported to the caller (the reconciler
// decides what a rejection means); this class never silently drops or
// retries anything on its own.
namespace truetest::footprint {

class ReorderWindow
{
public:
    explicit ReorderWindow(std::int64_t window_ns = 2'000'000'000LL) noexcept
        : window_ns_(window_ns > 0 ? window_ns : 1)
    {
    }

    // Buffers the trade if its event_ns is still within the window of the
    // highest event_ns seen so far (the watermark); returns false (does
    // NOT buffer it) if it's already older than watermark - window_ns.
    bool offer(const PublicTrade& trade)
    {
        if (has_watermark_ && trade.event_ns < watermark_ns_ - window_ns_)
            return false;

        if (!has_watermark_ || trade.event_ns > watermark_ns_)
        {
            watermark_ns_ = trade.event_ns;
            has_watermark_ = true;
        }
        buffer_.emplace(trade.event_ns, trade);
        return true;
    }

    // Emits (removes from the buffer, appends to `out`, ascending
    // event_ns) every trade now guaranteed safe: no future offer() could
    // ever accept something earlier, because offer()'s own watermark check
    // would reject it. Call after every offer() (or on an external tick
    // during a lull) to make forward progress.
    void drain_ready(std::vector<PublicTrade>& out)
    {
        if (!has_watermark_)
            return;
        const std::int64_t threshold = watermark_ns_ - window_ns_;
        auto it = buffer_.begin();
        while (it != buffer_.end() && it->first <= threshold)
        {
            out.push_back(it->second);
            it = buffer_.erase(it);
        }
    }

    // Force-emits everything still buffered, ascending event_ns - end of
    // stream / explicit flush.
    void flush(std::vector<PublicTrade>& out)
    {
        for (auto& [ts, trade] : buffer_)
            out.push_back(trade);
        buffer_.clear();
    }

    std::size_t size() const noexcept { return buffer_.size(); }
    bool empty() const noexcept { return buffer_.empty(); }
    std::int64_t watermark_ns() const noexcept { return watermark_ns_; }

private:
    std::int64_t window_ns_;
    std::int64_t watermark_ns_ = 0;
    bool has_watermark_ = false;
    std::multimap<std::int64_t, PublicTrade> buffer_;
};

} // namespace truetest::footprint
