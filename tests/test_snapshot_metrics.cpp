#include <gtest/gtest.h>

#include "ui/snapshot_metrics.h"

#include <limits>

using namespace truetest::ui;

TEST(SnapshotMetrics, AvailabilityRejectsMissingAndNonFiniteValues)
{
    EXPECT_FALSE(available_metric(false, 0.0));
    EXPECT_DOUBLE_EQ(*available_metric(true, 0.0), 0.0);
    EXPECT_FALSE(available_metric(
        true, std::numeric_limits<double>::quiet_NaN()));
}

TEST(SnapshotMetrics, PositionDerivativesRequireAuthoritativeInputs)
{
    dashboard_snapshot::position_row position{
        .symbol = "BTCUSDT", .qty = 2.0, .avg_entry = 100.0,
        .mark = 110.0, .unrealized = 20.0,
        .mark_available = false, .unrealized_available = false};
    EXPECT_FALSE(position_notional(position));
    EXPECT_FALSE(position_unrealized_pct(position));
    position.mark_available = true;
    position.unrealized_available = true;
    EXPECT_DOUBLE_EQ(*position_notional(position), 220.0);
    EXPECT_DOUBLE_EQ(*position_unrealized_pct(position), 10.0);
}

TEST(SnapshotMetrics, SnapshotAgeUsesProducerTimestampAndFailsClosed)
{
    using namespace std::chrono_literals;
    const auto now = std::chrono::steady_clock::now();
    dashboard_snapshot snapshot;

    EXPECT_TRUE(snapshot_is_stale(&snapshot, now, 1500ms));
    EXPECT_TRUE(snapshot_is_stale(nullptr, now, 1500ms));

    snapshot.generated_at_available = true;
    snapshot.generated_at = now - 1499ms;
    ASSERT_TRUE(snapshot_age(snapshot, now));
    EXPECT_EQ(*snapshot_age(snapshot, now), 1499ms);
    EXPECT_FALSE(snapshot_is_stale(&snapshot, now, 1500ms));

    snapshot.generated_at = now - 1501ms;
    EXPECT_TRUE(snapshot_is_stale(&snapshot, now, 1500ms));

    snapshot.generated_at = now + 1ms;
    EXPECT_FALSE(snapshot_age(snapshot, now));
    EXPECT_TRUE(snapshot_is_stale(&snapshot, now, 1500ms));
}

TEST(SnapshotMetrics, MissingConfiguredMetricCannotBeClassifiedSafe)
{
    dashboard_snapshot::risk_view risk;
    risk.daily_loss_limit = 100.0;
    risk.daily_loss_available = false;
    auto assessment = assess_snapshot_risk(risk);
    EXPECT_EQ(assessment.level, snapshot_risk_level::unknown);
    EXPECT_FALSE(assessment.complete);

    risk.daily_loss_available = true;
    risk.daily_loss = 30.0;
    EXPECT_EQ(assess_snapshot_risk(risk).level, snapshot_risk_level::caution);
    risk.daily_loss = 50.0;
    EXPECT_EQ(assess_snapshot_risk(risk).level, snapshot_risk_level::warning);
    risk.daily_loss = 70.0;
    EXPECT_EQ(assess_snapshot_risk(risk).level, snapshot_risk_level::danger);
    risk.daily_loss = 90.0;
    EXPECT_EQ(assess_snapshot_risk(risk).level, snapshot_risk_level::critical);
}
