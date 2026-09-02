#include "web/snapshot_json.h"
#include "ui/dashboard_snapshot.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <limits>

namespace {

using truetest::ui::dashboard_snapshot;

TEST(SnapshotJsonV3, UsesNullAndEffectiveAvailabilityForDashboardMetrics)
{
    dashboard_snapshot snapshot;
    snapshot.equity = 101.0;
    snapshot.equity_available = true;
    snapshot.total_pnl = std::numeric_limits<double>::infinity();
    snapshot.total_pnl_available = true;
    snapshot.realized_pnl = 4.0;
    snapshot.realized_pnl_available = false;
    snapshot.unrealized_pnl = -2.5;
    snapshot.unrealized_pnl_available = true;

    snapshot.positions.push_back({.symbol = "BTCUSDT",
                                  .qty = 1.0,
                                  .avg_entry = 100.0,
                                  .mark = std::numeric_limits<double>::quiet_NaN(),
                                  .unrealized = 2.0,
                                  .mark_available = true,
                                  .unrealized_available = false});

    snapshot.risk.daily_loss = 0.0;
    snapshot.risk.daily_loss_available = true;
    snapshot.risk.max_drawdown_pct = std::numeric_limits<double>::quiet_NaN();
    snapshot.risk.max_drawdown_available = true;
    snapshot.risk.exposure = 50.0;
    snapshot.risk.exposure_available = false;

    snapshot.trend.equity_now = 101.0;
    snapshot.trend.equity_change_pct = std::numeric_limits<double>::infinity();
    snapshot.trend.equity_available = true;
    snapshot.trend.drawdown_now_pct = 1.25;
    snapshot.trend.drawdown_now_available = false;

    snapshot.queue.available = false;
    snapshot.queue.avg_bps = 0;

    const auto json = nlohmann::json::parse(truetest::web::snapshot_to_json(snapshot));

    EXPECT_EQ(json.at("schema_version"), 3);
    EXPECT_TRUE(json.at("generated_at_ms").is_null());
    EXPECT_FALSE(json.at("generated_at_available"));
    EXPECT_EQ(json.at("account").at("equity"), 101.0);
    EXPECT_TRUE(json.at("account").at("equity_available"));
    EXPECT_TRUE(json.at("account").at("total_pnl").is_null());
    EXPECT_FALSE(json.at("account").at("total_pnl_available"));
    EXPECT_TRUE(json.at("account").at("realized_pnl").is_null());
    EXPECT_FALSE(json.at("account").at("realized_pnl_available"));
    EXPECT_EQ(json.at("account").at("unrealized_pnl"), -2.5);
    EXPECT_TRUE(json.at("account").at("unrealized_pnl_available"));

    const auto& position = json.at("positions").at(0);
    EXPECT_TRUE(position.at("mark").is_null());
    EXPECT_FALSE(position.at("mark_available"));
    EXPECT_TRUE(position.at("unrealized").is_null());
    EXPECT_FALSE(position.at("unrealized_available"));

    EXPECT_EQ(json.at("risk").at("daily_loss"), 0.0);
    EXPECT_TRUE(json.at("risk").at("daily_loss_available"));
    EXPECT_TRUE(json.at("risk").at("max_drawdown_pct").is_null());
    EXPECT_FALSE(json.at("risk").at("max_drawdown_available"));
    EXPECT_TRUE(json.at("risk").at("exposure").is_null());
    EXPECT_FALSE(json.at("risk").at("exposure_available"));

    EXPECT_EQ(json.at("trend").at("equity_now"), 101.0);
    EXPECT_TRUE(json.at("trend").at("equity_available"));
    EXPECT_TRUE(json.at("trend").at("equity_change_pct").is_null());
    EXPECT_FALSE(json.at("trend").at("equity_change_available"));
    EXPECT_TRUE(json.at("trend").at("drawdown_now_pct").is_null());
    EXPECT_FALSE(json.at("trend").at("drawdown_now_available"));

    EXPECT_TRUE(json.at("queue").at("avg_bps").is_null());
    EXPECT_FALSE(json.at("queue").at("available"));
}

TEST(SnapshotJsonV3, ProjectsMonotonicGenerationTimeAndRejectsFarFutureValue)
{
    using namespace std::chrono_literals;
    dashboard_snapshot snapshot;
    snapshot.generated_at = std::chrono::steady_clock::now() - 250ms;
    snapshot.generated_at_available = true;

    const auto before_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto json = nlohmann::json::parse(
        truetest::web::snapshot_to_json(snapshot));
    const auto after_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    ASSERT_TRUE(json.at("generated_at_available"));
    const auto generated_ms = json.at("generated_at_ms").get<long long>();
    EXPECT_GE(generated_ms, before_ms - 500);
    EXPECT_LE(generated_ms, after_ms);

    snapshot.generated_at = std::chrono::steady_clock::now() + 6s;
    const auto future_json = nlohmann::json::parse(
        truetest::web::snapshot_to_json(snapshot));
    EXPECT_TRUE(future_json.at("generated_at_ms").is_null());
    EXPECT_FALSE(future_json.at("generated_at_available"));
}

TEST(SnapshotJsonV3, ExposesStopTriggerAndNormalizesFillProvenance)
{
    dashboard_snapshot snapshot;
    snapshot.open_orders.push_back({.order_id = 1,
                                    .symbol = "BTCUSDT",
                                    .strategy_name = "test",
                                    .side = 'B',
                                    .type = 's',
                                    .qty = 1.0,
                                    .price = 99.0,
                                    .trigger_price = 100.0,
                                    .trigger_price_available = true,
                                    .age_seconds = 0,
                                    .status = "working"});
    snapshot.open_orders.push_back({.order_id = 2,
                                    .symbol = "ETHUSDT",
                                    .strategy_name = "test",
                                    .side = 'S',
                                    .type = 'S',
                                    .qty = 1.0,
                                    .price = 0.0,
                                    .trigger_price = std::numeric_limits<double>::infinity(),
                                    .trigger_price_available = true,
                                    .age_seconds = 0,
                                    .status = "working"});
    snapshot.recent_fills.push_back({.ts = {},
                                     .symbol = "BTCUSDT",
                                     .side = 'B',
                                     .qty = 1.0,
                                     .price = 1.0,
                                     .fee = 0.0,
                                     .source = "exchange"});
    snapshot.recent_fills.push_back({.ts = {},
                                     .symbol = "BTCUSDT",
                                     .side = 'B',
                                     .qty = 1.0,
                                     .price = 1.0,
                                     .fee = 0.0,
                                     .source = "simulated"});
    snapshot.recent_fills.push_back({.ts = {},
                                     .symbol = "BTCUSDT",
                                     .side = 'B',
                                     .qty = 1.0,
                                     .price = 1.0,
                                     .fee = 0.0,
                                     .source = "local"});
    snapshot.recent_fills.push_back({.ts = {},
                                     .symbol = "BTCUSDT",
                                     .side = 'B',
                                     .qty = 1.0,
                                     .price = 1.0,
                                     .fee = 0.0,
                                     .source = nullptr});

    const auto json = nlohmann::json::parse(truetest::web::snapshot_to_json(snapshot));

    EXPECT_EQ(json.at("open_orders").at(0).at("trigger_price"), 100.0);
    EXPECT_TRUE(json.at("open_orders").at(0).at("trigger_price_available"));
    EXPECT_TRUE(json.at("open_orders").at(1).at("trigger_price").is_null());
    EXPECT_FALSE(json.at("open_orders").at(1).at("trigger_price_available"));

    EXPECT_EQ(json.at("recent_fills").at(0).at("source"), "exchange");
    EXPECT_EQ(json.at("recent_fills").at(1).at("source"), "simulated");
    EXPECT_EQ(json.at("recent_fills").at(2).at("source"), "unknown");
    EXPECT_EQ(json.at("recent_fills").at(3).at("source"), "unknown");
}

}  // namespace
