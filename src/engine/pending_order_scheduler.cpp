#include "pending_order_scheduler.h"

// LIVE-SAFETY SURFACE: see pending_order_scheduler.h + scripts/check-live-safety-freeze.sh.

void PendingOrderScheduler::clear()
{
    while (!pending_orders_.empty()) pending_orders_.pop();
    bar_delayed_orders_.clear();
    bar_delayed_ready_.clear();
    order_seq_ = 0;
    day_order_ids_.clear();
}

void PendingOrderScheduler::reserve_bar_delay_capacity(std::size_t configured_max_open_orders)
{
    if (bar_delayed_orders_.capacity() == 0)
        bar_delayed_orders_.reserve(configured_max_open_orders);
    if (bar_delayed_ready_.capacity() < bar_delayed_orders_.capacity())
        bar_delayed_ready_.reserve(bar_delayed_orders_.capacity());
}

void PendingOrderScheduler::schedule_latency(std::shared_ptr<order_event> order, uint64_t seq)
{
    pending_orders_.push({std::move(order), seq});
}

bool PendingOrderScheduler::latency_due(std::chrono::system_clock::time_point sim_time) const
{
    return !pending_orders_.empty()
        && pending_orders_.top().order->get_earliest_eligible_ts() <= sim_time;
}

std::shared_ptr<order_event> PendingOrderScheduler::pop_due_latency()
{
    auto entry = pending_orders_.top();
    pending_orders_.pop();
    return entry.order;
}

void PendingOrderScheduler::schedule_bar_delay(std::shared_ptr<order_event> order, uint64_t seq,
                                               std::size_t remaining_symbol_events)
{
    bar_delayed_orders_.push_back({std::move(order), seq, remaining_symbol_events});
}

bool PendingOrderScheduler::compact_bar_delay_due(std::string_view event_symbol)
{
    const std::size_t delayed_count = bar_delayed_orders_.size();
    if (delayed_count > bar_delayed_ready_.capacity())
        return false;

    // Stable, allocation-free single pass. Survivors retain insertion order
    // in bar_delayed_orders_; due entries retain insertion order in the
    // prewarmed ready buffer. Verbatim former engine::drain_pending_orders
    // compaction loop.
    std::size_t survivor_count = 0;
    for (std::size_t read = 0; read < delayed_count; ++read)
    {
        auto& entry = bar_delayed_orders_[read];
        const bool same_symbol = entry.order
            && entry.order->get_symbol() == event_symbol;
        if (same_symbol && entry.remaining_symbol_events <= 1)
        {
            bar_delayed_ready_.push_back(std::move(entry));
            continue;
        }

        if (same_symbol)
            --entry.remaining_symbol_events;
        if (survivor_count != read)
            bar_delayed_orders_[survivor_count] = std::move(entry);
        ++survivor_count;
    }
    bar_delayed_orders_.resize(survivor_count);
    return true;
}

std::shared_ptr<order_event> PendingOrderScheduler::take_ready_order(std::size_t i)
{
    return std::move(bar_delayed_ready_[i].order);
}

void PendingOrderScheduler::retain_ready_suffix(std::size_t first_unsubmitted)
{
    const std::size_t remaining = bar_delayed_ready_.size() - first_unsubmitted;
    if (remaining == 0)
    {
        bar_delayed_ready_.clear();
        return;
    }
    if (remaining > bar_delayed_orders_.capacity() - bar_delayed_orders_.size())
        return;

    const std::size_t old_size = bar_delayed_orders_.size();
    bar_delayed_orders_.resize(old_size + remaining);
    std::size_t left = old_size;
    std::size_t right = bar_delayed_ready_.size();
    std::size_t out = old_size + remaining;
    while (left > 0 && right > first_unsubmitted)
    {
        if (bar_delayed_orders_[left - 1].seq
            > bar_delayed_ready_[right - 1].seq)
        {
            bar_delayed_orders_[--out] =
                std::move(bar_delayed_orders_[--left]);
        }
        else
        {
            bar_delayed_orders_[--out] =
                std::move(bar_delayed_ready_[--right]);
        }
    }
    while (right > first_unsubmitted)
    {
        bar_delayed_orders_[--out] =
            std::move(bar_delayed_ready_[--right]);
    }
    bar_delayed_ready_.clear();
}

void PendingOrderScheduler::mark_day_order(std::string symbol, uint64_t order_id)
{
    day_order_ids_.push_back({std::move(symbol), order_id});
}
