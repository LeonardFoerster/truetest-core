#include "analytics/footprint/footprint_reconciler.h"

#include "analytics/footprint/footprint_dedup.h"

#include <algorithm>

namespace truetest::footprint {

namespace {
bool by_event_ns(const PublicTrade& a, const PublicTrade& b) noexcept
{
    return a.event_ns < b.event_ns;
}
} // namespace

FootprintReconciler::FootprintReconciler() : FootprintReconciler(Config{}) {}

FootprintReconciler::FootprintReconciler(Config config)
    : config_(config)
    , reorder_(config.reorder_window_ns)
{
}

bool FootprintReconciler::try_append(std::vector<PublicTrade>& target, PublicTrade trade)
{
    if (!seen_keys_.insert(dedup_key_of(trade)).second)
        return false; // duplicate across cache/history/live - dropped, footprint.md §2.2
    target.push_back(std::move(trade));
    return true;
}

void FootprintReconciler::load_cache(std::vector<PublicTrade> cached_trades)
{
    cache_trades_ = cached_trades;

    seen_keys_.clear();
    std::vector<PublicTrade> published;
    published.reserve(cached_trades.size());
    for (auto& t : cached_trades)
        try_append(published, t);
    std::sort(published.begin(), published.end(), by_event_ns);

    verified_ = std::move(published);
    status_ = data_status::backfilling;
}

void FootprintReconciler::merge_and_publish(std::vector<PublicTrade> known_good)
{
    // Trades still inside their reorder grace period at this transition
    // point are real arrivals, not noise - flush them in too rather than
    // silently losing them (they'd otherwise sit in the reorder window
    // forever, since nothing drains it once we stop calling on_live_trade
    // in the pre-LIVE buffering mode).
    std::vector<PublicTrade> flushed;
    reorder_.flush(flushed);

    seen_keys_.clear();
    std::vector<PublicTrade> merged;
    merged.reserve(known_good.size() + live_buffer_.size() + flushed.size());
    for (auto& t : known_good)
        try_append(merged, t);
    for (auto& t : live_buffer_)
        try_append(merged, t);
    for (auto& t : flushed)
        try_append(merged, t);
    std::sort(merged.begin(), merged.end(), by_event_ns);

    verified_ = std::move(merged);
    live_buffer_.clear();
    gap_.reset();
    status_ = data_status::live;
}

void FootprintReconciler::complete_backfill(std::vector<PublicTrade> history_trades,
                                            bool contiguous_with_cache)
{
    if (!contiguous_with_cache)
    {
        on_fault(fault_kind::missing_overlap);
        return;
    }

    std::vector<PublicTrade> known_good = cache_trades_;
    known_good.insert(known_good.end(), history_trades.begin(), history_trades.end());
    merge_and_publish(std::move(known_good));
}

void FootprintReconciler::on_fault(fault_kind /*fault*/)
{
    if (status_ == data_status::recovering)
        return; // already frozen and buffering - idempotent, no churn
    status_ = data_status::recovering;
    gap_.reset(); // a precise gap is only known once repair_failed() reports one
}

void FootprintReconciler::repair_succeeded(std::vector<PublicTrade> repaired_trades)
{
    std::vector<PublicTrade> known_good = verified_; // last frozen-good result
    known_good.insert(known_good.end(), repaired_trades.begin(), repaired_trades.end());
    merge_and_publish(std::move(known_good));
}

void FootprintReconciler::repair_failed(std::int64_t gap_start_ns, std::int64_t gap_end_ns)
{
    status_ = data_status::partial;
    gap_ = std::make_pair(gap_start_ns, gap_end_ns);
    // verified_ is untouched - the gap is a hole alongside otherwise-good
    // data, never a silent rewrite of what's already published.
}

void FootprintReconciler::on_live_trade(const PublicTrade& trade)
{
    if (!reorder_.offer(trade))
    {
        on_fault(fault_kind::reorder_window_violation);
        return; // the too-late trade itself cannot be silently incorporated
    }

    std::vector<PublicTrade> ready;
    reorder_.drain_ready(ready);
    for (auto& t : ready)
    {
        if (status_ == data_status::live)
        {
            // Steady state - append straight to the published set. Safe
            // append order: drain_ready() only ever emits in non-decreasing
            // event_ns relative to earlier emissions from the same window,
            // so verified_ stays sorted without re-sorting on every trade.
            try_append(verified_, std::move(t));
            continue;
        }

        if (live_buffer_.size() >= config_.live_buffer_capacity)
        {
            on_fault(fault_kind::overflow);
            continue; // drop - "continue buffering within FIXED capacity"
        }
        live_buffer_.push_back(std::move(t));
    }
}

} // namespace truetest::footprint
