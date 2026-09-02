#include "dashboard_snapshot_builder.h"

#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace {

const char* canonical_order_status(const char* status) noexcept
{
    const std::string_view value = status ? status : "";
    for (const char* candidate : {
             "open", "submit_pending", "stop_pending", "latency_pending",
             "bar_delay_pending", "partial", "cancel_pending",
             "cancel_unknown", "cancel_refused", "cancel_failed"})
        if (value == candidate) return candidate;
    return "unknown";
}

std::uint64_t advance_epoch(std::atomic<std::uint64_t>& epoch) noexcept
{
    auto current = epoch.load(std::memory_order_relaxed);
    while (current != std::numeric_limits<std::uint64_t>::max())
    {
        if (epoch.compare_exchange_weak(
                current, current + 1U,
                std::memory_order_release,
                std::memory_order_relaxed))
            return current + 1U;
    }
    return current;
}

} // namespace

bool DashboardSnapshotBuilder::snapshot_dashboard(
    truetest::ui::dashboard_snapshot& out) const
{
    // Reading a frame also requests its successor. The epoch cannot be lost:
    // the producer only records the exact epoch it captured.
    advance_epoch(dashboard_request_epoch_);

    auto pin = pin_latest_projection();
    if (!pin) return false;

    // Materialize while the immutable projection is pinned. Rich strings and
    // vectors are cold reader work; no provider, adapter, persistence, or
    // event-owned container is consulted. Exceptions release the pin through
    // projection_pin's destructor.
    truetest::ui::dashboard_snapshot candidate;
    materialize_dashboard_view(pin->value(), candidate);
    pin.reset();

    // /proc sampling and its mutex are also reader-side and happen after
    // the projection pin is released, reducing producer back-pressure.
    sample_memory_if_due(candidate);
    out = std::move(candidate);
    return true;
}

std::optional<DashboardSnapshotBuilder::projection_pin>
DashboardSnapshotBuilder::pin_latest_projection() const noexcept
{
    for (;;)
    {
        const auto token =
            published_snapshot_token_.load(std::memory_order_acquire);
        if (token == kNoPublishedSnapshotToken) return std::nullopt;
        const auto index = static_cast<std::uint8_t>(token & 0xffU);
        if (index >= kSnapshotSlotCount) return std::nullopt;

        auto& slot = snapshot_slots_[index];
        auto state = slot.access_state.load(std::memory_order_acquire);
        bool pinned = false;
        while ((state & kSnapshotWriterOwned) == 0U)
        {
            if ((state & kSnapshotReaderMask) == kSnapshotReaderMask) break;
            if (slot.access_state.compare_exchange_weak(
                    state, state + 1U,
                    std::memory_order_acquire,
                    std::memory_order_relaxed))
            {
                pinned = true;
                break;
            }
        }
        if (!pinned) continue;

        projection_pin pin{slot.access_state, slot.value};
        if (published_snapshot_token_.load(std::memory_order_acquire) == token)
            return pin;
    }
}

void DashboardSnapshotBuilder::request_dashboard_refresh()
{
    const auto requested = advance_epoch(dashboard_request_epoch_);
    auto forced = dashboard_force_epoch_.load(std::memory_order_relaxed);
    while (forced < requested &&
           !dashboard_force_epoch_.compare_exchange_weak(
               forced, requested,
               std::memory_order_release,
               std::memory_order_relaxed)) {}
}

void DashboardSnapshotBuilder::begin_event_boundary() noexcept
{
    if (event_boundary_depth_++ == 0U) event_boundary_failed_ = false;
}

void DashboardSnapshotBuilder::end_event_boundary(
    bool normally_completed) noexcept
{
    if (event_boundary_depth_ == 0U) return;
    if (!normally_completed) event_boundary_failed_ = true;
    --event_boundary_depth_;
    if (event_boundary_depth_ != 0U) return;
    const bool committed = !event_boundary_failed_;
    event_boundary_failed_ = false;
    if (committed) (void)refresh_if_due();
}

bool DashboardSnapshotBuilder::publish_projection(
    bool bypass_cadence, bool analytics_quiescent) noexcept
{
    const auto requested =
        dashboard_request_epoch_.load(std::memory_order_acquire);
    const auto forced =
        dashboard_force_epoch_.load(std::memory_order_acquire) >
        dashboard_captured_epoch_;
    if (!bypass_cadence && requested <= dashboard_captured_epoch_) return false;

    const auto now = std::chrono::steady_clock::now();
    if (!bypass_cadence && !forced &&
        published_snapshot_token_.load(std::memory_order_acquire) !=
            kNoPublishedSnapshotToken &&
        now - dashboard_view_last_ < dashboard_view_interval_)
        return false;

    const auto published_token =
        published_snapshot_token_.load(std::memory_order_acquire);
    const auto published = published_token == kNoPublishedSnapshotToken
        ? kNoPublishedSnapshot
        : static_cast<std::uint8_t>(published_token & 0xffU);

    std::uint8_t target = kNoPublishedSnapshot;
    for (std::uint8_t offset = 1; offset <= kSnapshotSlotCount; ++offset)
    {
        const auto i = published == kNoPublishedSnapshot
            ? static_cast<std::uint8_t>(offset - 1U)
            : static_cast<std::uint8_t>(
                  (published + offset) % kSnapshotSlotCount);
        auto expected = std::uint32_t{0};
        if (snapshot_slots_[i].access_state.compare_exchange_strong(
                expected, kSnapshotWriterOwned,
                std::memory_order_acquire,
                std::memory_order_relaxed))
        {
            target = i;
            break;
        }
    }
    if (target == kNoPublishedSnapshot) return false;

    (void)capture_projection(snapshot_slots_[target].value, requested,
                             analytics_quiescent);
    ++snapshot_generation_;
    const auto token = (snapshot_generation_ << 8U) | target;
    // Publish the new generation while WRITER is still held. This also makes
    // it safe to reuse the currently-published slot when both older slots are
    // pinned: a reader of the old token cannot pin until it can observe and
    // reject the new token.
    published_snapshot_token_.store(token, std::memory_order_release);
    snapshot_slots_[target].access_state.store(0U, std::memory_order_release);
    dashboard_captured_epoch_ = requested;
    dashboard_view_last_ = now;
    return true;
}

bool DashboardSnapshotBuilder::refresh_if_due() noexcept
{
    return publish_projection(false, false);
}

bool DashboardSnapshotBuilder::publish_initial_snapshot() noexcept
{
    request_dashboard_refresh();
    return publish_projection(true, true);
}

bool DashboardSnapshotBuilder::publish_final_snapshot() noexcept
{
    request_dashboard_refresh();
    return publish_projection(true, true);
}

void DashboardSnapshotBuilder::cache_open_order(
    const order_event& order) noexcept
{
    open_order_cache_entry* target = nullptr;
    open_order_cache_entry* first_tombstone = nullptr;
    const auto mask = open_orders_cache_.size() - 1U;
    auto index = static_cast<std::size_t>(
        order.get_order_id() * 0x9e3779b97f4a7c15ULL) & mask;
    for (std::size_t probe = 0; probe < open_orders_cache_.size(); ++probe)
    {
        auto& entry = open_orders_cache_[index];
        if (entry.state == open_order_cache_entry::slot_state::occupied &&
            entry.row.order_id == order.get_order_id())
        {
            target = &entry;
            break;
        }
        if (entry.state == open_order_cache_entry::slot_state::tombstone &&
            first_tombstone == nullptr)
            first_tombstone = &entry;
        if (entry.state == open_order_cache_entry::slot_state::empty)
        {
            target = first_tombstone ? first_tombstone : &entry;
            break;
        }
        index = (index + 1U) & mask;
    }
    if (target == nullptr) target = first_tombstone;
    if (target == nullptr ||
        order.get_symbol().size() > target->row.symbol.capacity() ||
        order.get_strategy_name().size() >
            target->row.strategy_name.capacity())
    {
        dashboard_cache_complete_ = false;
        ++open_orders_cache_overflow_count_;
        return;
    }

    const bool newly_occupied =
        target->state != open_order_cache_entry::slot_state::occupied;
    if (newly_occupied &&
        open_orders_cache_size_ >= truetest::dashboard::kMaxOpenOrders)
    {
        dashboard_cache_complete_ = false;
        ++open_orders_cache_overflow_count_;
        return;
    }
    auto& entry = *target;
    entry.state = open_order_cache_entry::slot_state::occupied;
    entry.row.order_id = order.get_order_id();
    entry.row.symbol = order.get_symbol();
    entry.row.strategy_name = order.get_strategy_name();
    entry.row.side = order.get_side() == order_side::buy ? 'B' : 'S';
    switch (order.get_order_type())
    {
        case order_type::market: entry.row.type = 'M'; break;
        case order_type::limit: entry.row.type = 'L'; break;
        case order_type::stop: entry.row.type = 'S'; break;
        case order_type::stop_limit: entry.row.type = 's'; break;
    }
    entry.row.qty = order.get_quantity();
    entry.row.price = order.get_price();
    entry.row.trigger_price = order.get_stop_price();
    entry.row.trigger_price_available =
        (order.get_order_type() == order_type::stop ||
         order.get_order_type() == order_type::stop_limit) &&
        std::isfinite(entry.row.trigger_price) &&
        entry.row.trigger_price > 0.0;
    entry.row.status = "open";
    entry.ts = order.get_timestamp();
    if (newly_occupied) ++open_orders_cache_size_;
}

void DashboardSnapshotBuilder::update_open_order_status(
    std::uint64_t id, const char* status) noexcept
{
    const auto mask = open_orders_cache_.size() - 1U;
    auto index = static_cast<std::size_t>(
        id * 0x9e3779b97f4a7c15ULL) & mask;
    for (std::size_t probe = 0; probe < open_orders_cache_.size(); ++probe)
    {
        auto& entry = open_orders_cache_[index];
        if (entry.state == open_order_cache_entry::slot_state::empty) return;
        if (entry.state == open_order_cache_entry::slot_state::occupied &&
            entry.row.order_id == id)
        {
            entry.row.status = canonical_order_status(status);
            return;
        }
        index = (index + 1U) & mask;
    }
}

void DashboardSnapshotBuilder::erase_open_order(std::uint64_t id) noexcept
{
    const auto mask = open_orders_cache_.size() - 1U;
    auto index = static_cast<std::size_t>(
        id * 0x9e3779b97f4a7c15ULL) & mask;
    for (std::size_t probe = 0; probe < open_orders_cache_.size(); ++probe)
    {
        auto& entry = open_orders_cache_[index];
        if (entry.state == open_order_cache_entry::slot_state::empty) return;
        if (entry.state == open_order_cache_entry::slot_state::occupied &&
            entry.row.order_id == id)
        {
            entry.state = open_order_cache_entry::slot_state::tombstone;
            --open_orders_cache_size_;
            return;
        }
        index = (index + 1U) & mask;
    }
}

void DashboardSnapshotBuilder::cache_fill(const fill_event& fill) noexcept
{
    auto& row = recent_fills_cache_[recent_fills_head_];
    if (fill.get_symbol().size() > row.symbol.capacity())
    {
        dashboard_cache_complete_ = false;
        return;
    }
    row.ts = fill.get_timestamp();
    row.symbol = fill.get_symbol();
    row.side = fill.get_side() == order_side::buy ? 'B' : 'S';
    row.qty = fill.get_filled_quantity();
    row.price = fill.get_fill_price();
    row.fee = fill.get_commission();
    switch (fill.get_source())
    {
        case fill_source::exchange: row.source = "exchange"; break;
        case fill_source::simulated: row.source = "simulated"; break;
        case fill_source::unknown: row.source = "unknown"; break;
    }
    recent_fills_head_ = (recent_fills_head_ + 1U) % kRecentFillsCap;
    if (recent_fills_count_ < kRecentFillsCap) ++recent_fills_count_;
}

void DashboardSnapshotBuilder::clear_for_mc_reset()
{
    for (auto& entry : open_orders_cache_)
        entry.state = open_order_cache_entry::slot_state::empty;
    open_orders_cache_size_ = 0;
    open_orders_cache_overflow_count_ = 0;
    recent_fills_count_ = 0;
    recent_fills_head_ = 0;
    dashboard_cache_complete_ = true;
    published_snapshot_token_.store(
        kNoPublishedSnapshotToken, std::memory_order_release);
    dashboard_captured_epoch_ = 0;
    request_dashboard_refresh();
    event_boundary_depth_ = 0;
    event_boundary_failed_ = false;
    {
        std::lock_guard<std::mutex> lock(memory_cache_mu_);
        memory_cache_initialised_ = false;
    }
}
