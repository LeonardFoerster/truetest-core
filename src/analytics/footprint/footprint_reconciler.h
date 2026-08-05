#pragma once

#include "analytics/footprint/footprint_dedup.h"
#include "analytics/footprint/footprint_reorder_window.h"
#include "types/footprint_data_status.h"
#include "types/public_trade.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

// footprint.md §2.2's reconciliation state machine: BACKFILLING → LIVE,
// with RECOVERING → (repaired) LIVE or → STALE/PARTIAL on faults. This
// class owns the MERGE/DEDUP/STATE logic only - it does not perform any
// I/O itself. The cache read/write (a future segment-cache layer) and REST
// history fetch (Phase 4b) are the cold worker's job; they hand this class
// already-fetched trade vectors and a verdict on contiguity, matching how
// FootprintAggregator (§2.2's other half) stays I/O-free too.
//
// Threading: single cold-worker-owned, like FootprintAggregator - no
// internal synchronization. on_live_trade()/complete_backfill()/etc. must
// all be called from the same thread.
namespace truetest::footprint {

class FootprintReconciler
{
public:
    struct Config
    {
        std::int64_t reorder_window_ns = 2'000'000'000LL; // footprint.md §2.2: two seconds
        std::size_t live_buffer_capacity = 65536;         // "buffer ... within fixed capacity"
    };

    // Split into two overloads rather than `Config config = {}` - this
    // GCC (16.1.1) rejects a default argument of a type nested inside the
    // same class as the constructor ("could not convert {} to Config"),
    // even though Config{} is perfectly well-formed as an ordinary
    // expression (see the delegating constructor below).
    FootprintReconciler();
    explicit FootprintReconciler(Config config);

    // §2.2 "start live capture first, buffer incoming trades." Call for
    // every trade arriving from the live tap, in any state. Routes through
    // the reorder window; a rejection (arrival older than the window) is
    // itself the "arrival older than the reorder window" fault and enters
    // RECOVERING. Once LIVE, ready trades are appended straight to the
    // published set instead of held in the pending buffer.
    void on_live_trade(const PublicTrade& trade);

    // §2.2 "load verified cache immediately, and publish it as
    // BACKFILLING." `cached_trades` is assumed already the cache layer's
    // verified, contiguous range - this dedupes/sorts it defensively and
    // publishes it immediately.
    void load_cache(std::vector<PublicTrade> cached_trades);

    // §2.2 steps 4-8: supply history fetched up to the live-start
    // watermark. `contiguous_with_cache` is the caller's verdict (a real
    // REST client, Phase 4b, determines this - missing overlap is a fault
    // this class only reacts to). On success: dedupes cache+history+
    // buffered-live, orders by event_ns, and atomically becomes the new
    // verified LIVE result. On failure: enters RECOVERING via the
    // "missing_overlap" fault, freezing whatever was last verified.
    void complete_backfill(std::vector<PublicTrade> history_trades, bool contiguous_with_cache);

    enum class fault_kind : std::uint8_t
    {
        disconnect,
        overflow,
        corrupt_segment,
        missing_overlap,
        reorder_window_violation,
    };

    // §2.2 fault path: freezes the last verified contiguous result
    // (already implicit - nothing here ever mutates verified_trades()
    // in place), keeps buffering live trades (bounded), enters RECOVERING.
    // Idempotent while already RECOVERING - repeated fault signals (e.g.
    // overflow firing on every subsequent trade) do not churn state.
    void on_fault(fault_kind fault);

    // §2.2 "attempt bounded cold-path repair ... atomically publish the
    // repaired range." The cold worker drives the actual repair fetch
    // (rate limits, Retry-After); this only records success, merging the
    // repaired range with whatever was frozen plus anything buffered live
    // since, and returning to LIVE.
    void repair_succeeded(std::vector<PublicTrade> repaired_trades);

    // §2.2 "... or remain STALE/PARTIAL with the exact gap shown."
    void repair_failed(std::int64_t gap_start_ns, std::int64_t gap_end_ns);

    data_status status() const noexcept { return status_; }

    // Last verified, deduped, time-ordered trade sequence. footprint.md
    // "never mutate already-published historical bars silently" - only
    // load_cache()/complete_backfill()/repair_succeeded()/on_live_trade()
    // (LIVE-state append only) ever change this, and always by wholesale
    // replacement or pure append, never in-place edit of an existing entry.
    const std::vector<PublicTrade>& verified_trades() const noexcept { return verified_; }

    // Set only while status() == partial - the exact interval repair_failed()
    // reported as unrecoverable this attempt.
    std::optional<std::pair<std::int64_t, std::int64_t>> gap() const noexcept { return gap_; }

    std::size_t pending_live_count() const noexcept { return live_buffer_.size(); }

private:
    void enter_recovering();
    void merge_and_publish(std::vector<PublicTrade> known_good);
    bool try_append(std::vector<PublicTrade>& target, PublicTrade trade);

    Config config_;
    data_status status_ = data_status::unavailable;

    std::vector<PublicTrade> cache_trades_;
    std::vector<PublicTrade> verified_;
    std::vector<PublicTrade> live_buffer_; // pending merge - not yet published
    std::optional<std::pair<std::int64_t, std::int64_t>> gap_;

    ReorderWindow reorder_;
    std::unordered_set<TradeDedupKey> seen_keys_;
};

} // namespace truetest::footprint
