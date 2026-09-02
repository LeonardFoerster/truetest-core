#pragma once

// PendingOrderScheduler — narrow leaf collaborator (engine-decomposition
// preparatory extraction; see core/docs/internal/engine-decomposition.md
// "Phase 3 Candidate work" and the OrderIntentProcessor Preparation Report
// §8, which scoped this collaborator to latency + bar-delay scheduling only
// — NOT pending_stops_, whose trigger-condition evaluation stays woven into
// order-domain logic and is owned by OrderIntentProcessor (its sole reader
// and writer as of the Phase 3 extraction), not by this class.
//
// Owns exactly: the latency-model due-time queue, the bar-delay /
// same-symbol-observation-delay buffers, the shared monotonic sequence
// counter, and the retained DAY-TIF order id list. Every method here is a
// mechanical (behavior-preserving) relocation of what was
// engine::pending_orders_ / bar_delayed_orders_ / bar_delayed_ready_ /
// order_seq_ / day_order_ids_ plus the pure sequencing halves of
// engine::drain_pending_orders / drain_final_pending in engine_pending.cpp.
//
// Deliberately zero dependencies beyond core/event.h: no engine, no
// RiskManager, no Portfolio, no ExecutionRouter, no Provider, no
// FillProcessor, no Dashboard, no QuestDB — it never submits an order,
// never logs, never publishes, never triggers a halt. Every decision that
// requires those (capacity-exhausted rejection audit, halt on retained
// state, the actual submit call) is returned to the caller as a plain query
// / return value instead; the caller (OrderIntentProcessor, since the Phase
// 3 extraction) makes the call.
//
// LIVE-SAFETY SURFACE: this file carries logic previously inside frozen
// engine_orders.cpp / engine_pending.cpp. See scripts/check-live-safety-freeze.sh.

#include "core/event.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class PendingOrderScheduler final
{
public:
    // ---- lifecycle -----------------------------------------------------
    // Former engine::clear_pending_state's pending-order-scheduling part
    // (pending_stops_ and the L2 scratch buffers are NOT scheduler concerns;
    // OrderIntentProcessor owns the former, while engine owns the latter.
    // See the Preparation Report §3/§8.)
    void clear();

    // Former: `if (bar_delayed_orders_.capacity() == 0) bar_delayed_orders_.reserve(configured);`
    // + the paired bar_delayed_ready_ reserve. `configured_max_open_orders`
    // is a plain size_t computed by the caller from config_.risk.max_open_orders
    // (or DEFAULT_RING_SIZE) — the scheduler never reads RiskManager/config
    // itself. Idempotent: only reserves the first time (capacity()==0),
    // exactly like the original.
    void reserve_bar_delay_capacity(std::size_t configured_max_open_orders);

    // ---- latency queue (former pending_orders_) -------------------------
    // Shared monotonic tie-breaker for both queues below — former order_seq_,
    // incremented at every schedule_latency/schedule_bar_delay call site
    // regardless of which queue receives the order (mirrors the original
    // single shared counter exactly).
    uint64_t next_seq() noexcept { return order_seq_++; }

    void schedule_latency(std::shared_ptr<order_event> order, uint64_t seq);
    bool latency_due(std::chrono::system_clock::time_point sim_time) const;
    // Returns empty if the queue is empty. Otherwise copies-then-pops the top
    // entry and moves its order out, preserving the original valid-input order.
    std::shared_ptr<order_event> pop_due_latency();

    // ---- bar-delay scheduling (former route_order's execution_bar_delay branch) ----
    // Former: `bar_delayed_orders_.size() == bar_delayed_orders_.capacity()`.
    // Caller checks this BEFORE calling schedule_bar_delay and handles the
    // capacity-exhausted rejection (audit + rejection event) itself — the
    // scheduler never rejects/audits on its own.
    bool bar_delay_capacity_exhausted() const noexcept
    {
        return bar_delayed_orders_.size() == bar_delayed_orders_.capacity();
    }
    void schedule_bar_delay(std::shared_ptr<order_event> order, uint64_t seq,
                            std::size_t remaining_symbol_events);

    // ---- bar-delay due drain (former drain_pending_orders's compaction+ready loop) ----
    // Former: `if (!bar_delayed_ready_.empty()) { trigger_halt(...); ... }`.
    // Caller must check this BEFORE calling compact_bar_delay_due and halt
    // if true — a non-empty ready buffer means a prior drain was abandoned
    // mid-submit after a halt; never resume or silently discard it.
    bool has_retained_ready() const noexcept { return !bar_delayed_ready_.empty(); }

    // Former: the capacity pre-check (`delayed_count > bar_delayed_ready_.capacity()`)
    // plus the stable, allocation-free single-pass compaction. Returns false
    // (nothing mutated) iff the ready buffer's bounded capacity would be
    // exceeded — the caller then triggers the "ready capacity exhausted"
    // halt itself, matching the original message/ordering exactly.
    bool compact_bar_delay_due(std::string_view event_symbol);

    std::size_t ready_count() const noexcept { return bar_delayed_ready_.size(); }
    // Returns empty when i is out of range. Otherwise moves the shared_ptr out
    // of ready slot i (leaves that slot's pointer
    // null; the slot itself is discarded by the next clear_ready()/retain_
    // ready_suffix() call) — former `auto order = std::move(bar_delayed_ready_[i].order);`.
    std::shared_ptr<order_event> take_ready_order(std::size_t i);

    // Former restore_ready_suffix(first_unsubmitted) verbatim: merges every
    // not-yet-submitted ready entry back into the delayed buffer in global
    // seq order when bounded capacity permits (linear in-place merge, zero
    // allocation); if capacity does not permit it, deliberately leaves the
    // suffix (and, per the original, any still-unconsumed already-submitted
    // prefix) resident in the ready buffer for the terminal EOS expiry path
    // instead of growing or dropping it. An invalid index also leaves the
    // ready buffer intact so the caller can fail closed without losing orders.
    void retain_ready_suffix(std::size_t first_unsubmitted);
    void clear_ready() noexcept { bar_delayed_ready_.clear(); }

    // ---- day orders (TIF=day retention for EOS cancel_day_orders) -------
    void mark_day_order(std::string symbol, uint64_t order_id);
    const std::vector<std::pair<std::string, uint64_t>>& day_orders() const noexcept
    {
        return day_order_ids_;
    }
    void clear_day_orders() noexcept { day_order_ids_.clear(); }

    // ---- EOS expiry enumeration (former drain_final_pending) ------------
    // Calls expire_fn(const std::shared_ptr<order_event>&) once per never-
    // submitted candidate, in the exact original order (latency queue
    // pop-all, then surviving bar-delayed orders, then any retained ready
    // entries), then clears both buffers. Template, not std::function: zero
    // allocation, no type erasure, matches this repo's hot-path callback
    // preference.
    template <typename ExpireFn>
    void expire_all(ExpireFn&& expire_fn)
    {
        while (!pending_orders_.empty())
        {
            auto entry = pending_orders_.top();
            pending_orders_.pop();
            expire_fn(entry.order);
        }
        for (const auto& entry : bar_delayed_orders_)
            expire_fn(entry.order);
        bar_delayed_orders_.clear();
        for (const auto& entry : bar_delayed_ready_)
            expire_fn(entry.order);
        bar_delayed_ready_.clear();
    }

private:
    struct pending_entry
    {
        std::shared_ptr<order_event> order;
        uint64_t seq;
    };
    static bool pending_cmp(const pending_entry& a, const pending_entry& b)
    {
        if (a.order->get_earliest_eligible_ts() != b.order->get_earliest_eligible_ts())
            return a.order->get_earliest_eligible_ts() > b.order->get_earliest_eligible_ts();
        return a.seq > b.seq;
    }
    std::priority_queue<pending_entry, std::vector<pending_entry>,
                        decltype(&PendingOrderScheduler::pending_cmp)>
        pending_orders_{&PendingOrderScheduler::pending_cmp};

    struct bar_delayed_entry
    {
        std::shared_ptr<order_event> order;
        uint64_t seq;
        std::size_t remaining_symbol_events;
    };
    std::vector<bar_delayed_entry> bar_delayed_orders_;
    std::vector<bar_delayed_entry> bar_delayed_ready_;

    uint64_t order_seq_ = 0;
    std::vector<std::pair<std::string, uint64_t>> day_order_ids_;
};
