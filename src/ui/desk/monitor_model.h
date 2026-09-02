#pragma once

#include "ui/console_dashboard.h"
#include "ui/dashboard_snapshot.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace truetest::ui::desk {

// UI-thread-owned adapter for the ConsoleDashboard atomics. The console's
// rate_ema()/rate_tail() storage is render-thread-affine, so the ImGui desk
// derives its own rate from successive atomic event-count samples.
class MonitorTelemetry
{
public:
    using clock = std::chrono::steady_clock;

    bool merge(dashboard_snapshot& snap, const ConsoleDashboard* console, clock::time_point now)
    {
        if (!console) {
            available_ = false;
            have_rate_sample_ = false;
            rate_available_ = false;
            rate_ema_ = 0.0;
            return false;
        }

        const auto& stats = console->stats();
        const auto events = stats.events_total.load(std::memory_order_relaxed);
        const auto fills = stats.fills_total.load(std::memory_order_relaxed);
        const auto trades = stats.trades_total.load(std::memory_order_relaxed);

        snap.health.events_total = static_cast<std::size_t>(events);
        snap.health.fills_total = static_cast<std::size_t>(fills);
        snap.health.trades_total = static_cast<std::size_t>(trades);
        const auto drops_logging = stats.ring_drops_logging.load(std::memory_order_relaxed);
        const auto drops_risk = stats.ring_drops_risk.load(std::memory_order_relaxed);
        const auto drops_stats = stats.ring_drops_stats.load(std::memory_order_relaxed);
        const auto drops_observer = stats.ring_drops_observer.load(std::memory_order_relaxed);
        const auto drops_risk_stats = stats.ring_drops_risk_stats.load(std::memory_order_relaxed);
        const auto drops_mm = stats.ring_drops_mm.load(std::memory_order_relaxed);
        snap.health.ring_drops_logging = drops_logging;
        snap.health.ring_drops_risk = drops_risk;
        snap.health.ring_drops_stats = drops_stats;
        snap.health.ring_drops_observer = drops_observer;
        snap.health.ring_drops_risk_stats = drops_risk_stats;
        snap.health.ring_drops_mm = drops_mm;

        // The engine's try_push overflow counters are authoritative; the
        // RingBuffer drop_count shown by the builder tracks a different
        // DropOldest policy. Overlay by the existing ring names for the desk.
        for (auto& ring : snap.debug.rings) {
            const std::string_view name = ring.name ? ring.name : "";
            if (name == "logging")
                ring.drops = drops_logging;
            else if (name == "risk")
                ring.drops = drops_risk;
            else if (name == "stats")
                ring.drops = drops_stats;
            else if (name == "observer")
                ring.drops = drops_observer;
            else if (name == "risk_stats")
                ring.drops = drops_risk_stats;
            else if (name == "mm_event")
                ring.drops = drops_mm;
        }

        stream_state_ = static_cast<connection_state>(stats.state.load(std::memory_order_acquire));
        snap.risk.halted = snap.risk.halted || stats.halt_flag.load(std::memory_order_acquire);

        if (!have_rate_sample_ || events < last_events_ || now <= last_rate_sample_) {
            rate_ema_ = 0.0;
            have_rate_sample_ = true;
            rate_available_ = false;
        } else {
            const double seconds = std::chrono::duration<double>(now - last_rate_sample_).count();
            const double instantaneous = static_cast<double>(events - last_events_) / seconds;
            constexpr double alpha = 0.25;
            rate_ema_ = (rate_ema_ == 0.0) ? instantaneous
                                           : alpha * instantaneous + (1.0 - alpha) * rate_ema_;
            rate_available_ = true;
        }

        last_events_ = events;
        last_rate_sample_ = now;
        snap.health.rate_ev_per_sec = rate_ema_;
        available_ = true;
        return true;
    }

    bool available() const { return available_; }
    bool rate_available() const { return rate_available_; }
    connection_state stream_state() const { return stream_state_; }

private:
    bool available_ = false;
    bool have_rate_sample_ = false;
    bool rate_available_ = false;
    std::uint64_t last_events_ = 0;
    clock::time_point last_rate_sample_{};
    double rate_ema_ = 0.0;
    connection_state stream_state_ = connection_state::idle;
};

}  // namespace truetest::ui::desk
