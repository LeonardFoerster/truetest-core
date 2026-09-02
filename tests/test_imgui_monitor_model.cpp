#include <gtest/gtest.h>

#include "ui/desk/monitor_model.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace {

truetest::ui::ConsoleDashboard make_console()
{
    truetest::ui::dashboard_config config;
    config.mode = truetest::ui::output_mode::off;
    return truetest::ui::ConsoleDashboard(std::move(config));
}

} // namespace

TEST(ImGuiMonitorModel, MergesAtomicTelemetryAndComputesDeskLocalRate)
{
    auto console = make_console();
    auto& stats = console.stats();
    stats.events_total.store(100, std::memory_order_relaxed);
    stats.fills_total.store(7, std::memory_order_relaxed);
    stats.trades_total.store(3, std::memory_order_relaxed);
    stats.ring_drops_logging.store(1, std::memory_order_relaxed);
    stats.ring_drops_risk.store(2, std::memory_order_relaxed);
    stats.ring_drops_stats.store(3, std::memory_order_relaxed);
    stats.ring_drops_observer.store(4, std::memory_order_relaxed);
    stats.ring_drops_risk_stats.store(5, std::memory_order_relaxed);
    stats.ring_drops_mm.store(6, std::memory_order_relaxed);
    stats.state.store(
        static_cast<std::uint8_t>(truetest::ui::connection_state::live),
        std::memory_order_release);
    stats.halt_flag.store(true, std::memory_order_release);

    truetest::ui::dashboard_snapshot snap;
    snap.health.provider_name = "fixture-provider";
    snap.health.provider_state = 2;
    snap.health.avg_tick_to_trade_us = 17.5;
    snap.debug.rings = {
        {"logging", 0, 0, 16, 0},
        {"risk", 0, 0, 16, 0},
        {"stats", 0, 0, 16, 0},
        {"observer", 0, 0, 16, 0},
        {"risk_stats", 0, 0, 16, 0},
        {"mm_event", 0, 0, 16, 0},
    };
    truetest::ui::desk::MonitorTelemetry telemetry;
    const auto t0 = truetest::ui::desk::MonitorTelemetry::clock::time_point{
        std::chrono::seconds{10}};

    ASSERT_TRUE(telemetry.merge(snap, &console, t0));
    EXPECT_TRUE(telemetry.available());
    EXPECT_FALSE(telemetry.rate_available());
    EXPECT_EQ(telemetry.stream_state(), truetest::ui::connection_state::live);
    EXPECT_EQ(snap.health.events_total, 100u);
    EXPECT_EQ(snap.health.fills_total, 7u);
    EXPECT_EQ(snap.health.trades_total, 3u);
    EXPECT_EQ(snap.health.ring_drops_logging, 1u);
    EXPECT_EQ(snap.health.ring_drops_risk, 2u);
    EXPECT_EQ(snap.health.ring_drops_stats, 3u);
    EXPECT_EQ(snap.health.ring_drops_observer, 4u);
    EXPECT_EQ(snap.health.ring_drops_risk_stats, 5u);
    EXPECT_EQ(snap.health.ring_drops_mm, 6u);
    EXPECT_EQ(snap.health.provider_name, "fixture-provider");
    EXPECT_EQ(snap.health.provider_state, 2);
    EXPECT_DOUBLE_EQ(snap.health.avg_tick_to_trade_us, 17.5);
    EXPECT_TRUE(snap.risk.halted);
    EXPECT_DOUBLE_EQ(snap.health.rate_ev_per_sec, 0.0);

    ASSERT_EQ(snap.debug.rings.size(), 6u);
    for (std::size_t i = 0; i < snap.debug.rings.size(); ++i)
        EXPECT_EQ(snap.debug.rings[i].drops, i + 1);

    stats.events_total.store(150, std::memory_order_relaxed);
    ASSERT_TRUE(telemetry.merge(snap, &console, t0 + std::chrono::milliseconds{500}));
    EXPECT_TRUE(telemetry.rate_available());
    EXPECT_DOUBLE_EQ(snap.health.rate_ev_per_sec, 100.0);
}

TEST(ImGuiMonitorModel, RebaselinesRateWhenCounterResetsOrTimeDoesNotAdvance)
{
    auto console = make_console();
    auto& stats = console.stats();
    truetest::ui::dashboard_snapshot snap;
    truetest::ui::desk::MonitorTelemetry telemetry;
    const auto t0 = truetest::ui::desk::MonitorTelemetry::clock::time_point{
        std::chrono::seconds{20}};

    stats.events_total.store(100, std::memory_order_relaxed);
    ASSERT_TRUE(telemetry.merge(snap, &console, t0));
    stats.events_total.store(200, std::memory_order_relaxed);
    ASSERT_TRUE(telemetry.merge(snap, &console, t0 + std::chrono::seconds{1}));
    EXPECT_DOUBLE_EQ(snap.health.rate_ev_per_sec, 100.0);

    stats.events_total.store(10, std::memory_order_relaxed);
    ASSERT_TRUE(telemetry.merge(snap, &console, t0 + std::chrono::seconds{2}));
    EXPECT_FALSE(telemetry.rate_available());
    EXPECT_DOUBLE_EQ(snap.health.rate_ev_per_sec, 0.0);

    stats.events_total.store(20, std::memory_order_relaxed);
    ASSERT_TRUE(telemetry.merge(snap, &console, t0 + std::chrono::seconds{2}));
    EXPECT_FALSE(telemetry.rate_available());
    EXPECT_DOUBLE_EQ(snap.health.rate_ev_per_sec, 0.0);

    stats.events_total.store(30, std::memory_order_relaxed);
    ASSERT_TRUE(telemetry.merge(snap, &console, t0 + std::chrono::seconds{3}));
    EXPECT_TRUE(telemetry.rate_available());
    EXPECT_DOUBLE_EQ(snap.health.rate_ev_per_sec, 10.0);

    EXPECT_FALSE(telemetry.merge(snap, nullptr, t0 + std::chrono::seconds{4}));
    EXPECT_FALSE(telemetry.available());
    EXPECT_FALSE(telemetry.rate_available());
}
