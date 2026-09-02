#include <gtest/gtest.h>

#include "ui/dashboard_snapshot.h"
#include "ui/desk/desk_format.h"
#include "ui/desk/desk_state.h"
#include "ui/desk/desk_trade_actions.h"
#include "ui/desk/desk_view_model.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace {

using truetest::ui::dashboard_snapshot;
using namespace truetest::ui::desk;

dashboard_snapshot command_center_fixture()
{
    dashboard_snapshot snapshot;
    snapshot.cash = 9'900.0;
    snapshot.initial_balance = 10'000.0;
    snapshot.equity = 10'150.0;
    snapshot.equity_available = true;
    snapshot.total_pnl = 150.0;
    snapshot.total_pnl_available = true;
    snapshot.realized_pnl = 50.0;
    snapshot.realized_pnl_available = true;
    snapshot.unrealized_pnl = 100.0;
    snapshot.unrealized_pnl_available = true;
    snapshot.risk.exposure = 2'025.0;
    snapshot.risk.exposure_available = true;
    snapshot.risk.max_drawdown_pct = 3.5;
    snapshot.risk.max_drawdown_available = true;
    snapshot.trend.drawdown_now_pct = 1.25;
    snapshot.trend.drawdown_now_available = true;

    snapshot.market_rows = {
        {
            .symbol = "BTCUSDT",
            .mark = 101.0,
            .mark_available = true,
            .best_bid = 100.0,
            .best_ask = 102.0,
            .best_bid_available = true,
            .best_ask_available = true,
            .mid = 101.0,
            .spread = 2.0,
            .spread_bps = 198.0198,
            .microprice = 101.25,
            .imbalance = 0.2,
            .bbo_available = true,
            .microprice_available = true,
            .imbalance_available = true,
            .position_qty = 2.0,
            .working_buy_orders = 1,
            .working_sell_orders = 2,
        },
        {
            .symbol = "ETHUSDT",
            .mark = 20.0,
            .mark_available = true,
            .position_qty = -1.0,
        },
    };
    snapshot.positions = {
        {.symbol = "BTCUSDT",
         .qty = 2.0,
         .avg_entry = 96.0,
         .mark = 101.0,
         .unrealized = 10.0,
         .mark_available = true,
         .unrealized_available = true},
        {.symbol = "ETHUSDT",
         .qty = -1.0,
         .avg_entry = 20.0,
         .mark_available = false,
         .unrealized_available = false},
    };
    snapshot.open_orders = {
        {.order_id = 7,
         .symbol = "BTCUSDT",
         .strategy_name = "maker",
         .side = 'B',
         .type = 'L',
         .qty = 1.0,
         .price = 99.0,
         .age_seconds = 12,
         .status = "open"},
        {.order_id = 8,
         .symbol = "ETHUSDT",
         .strategy_name = "hedge",
         .side = 'S',
         .type = 'M',
         .qty = 1.0,
         .price = 0.0,
         .age_seconds = 1,
         .status = "submit_pending"},
    };
    snapshot.brackets = {
        {.opener_order_id = 7,
         .strategy_name = "maker",
         .symbol = "BTCUSDT",
         .side = 'L',
         .qty = 2.0,
         .entry_price = 96.0,
         .stop_loss = 90.0,
         .take_profit = 110.0,
         .mark = 101.0,
         .venue_managed = true,
         .venue_list_id = "venue-1",
         .age_seconds = 60},
        {.opener_order_id = 9,
         .strategy_name = "hedge",
         .symbol = "ETHUSDT",
         .side = 'S',
         .qty = 1.0,
         .entry_price = 20.0,
         .stop_loss = std::nullopt,
         .take_profit = std::nullopt,
         .mark = 20.0,
         .venue_list_id = "",
         .age_seconds = 0},
    };
    return snapshot;
}

}  // namespace

TEST(CommandCenterViewModel, ProjectsMarketWatchAndSelectionWithoutEngineAccess)
{
    auto snapshot = command_center_fixture();
    DeskState state;
    state.selected_symbol = "BTCUSDT";
    state.market_sort = MarketWatchSort::position;
    state.market_sort_descending = true;

    const auto view = build_command_center_view_model(snapshot, state);
    ASSERT_EQ(view.market_watch.size(), 2u);
    EXPECT_EQ(view.market_watch.front().symbol, "BTCUSDT");
    EXPECT_TRUE(view.market_watch.front().selected);
    EXPECT_EQ(view.market_watch.front().working_buy_orders, 1u);
    EXPECT_EQ(view.market_watch.front().working_sell_orders, 2u);
    EXPECT_TRUE(view.market_watch.front().microprice.has_value());
    ASSERT_TRUE(view.market_watch.front().imbalance_pct.has_value());
    EXPECT_DOUBLE_EQ(*view.market_watch.front().imbalance_pct, 20.0);
    EXPECT_FALSE(view.market_watch[1].bid.has_value());
}

TEST(CommandCenterViewModel, NonFinitePositionSortKeysAreUnavailableAndDeterministic)
{
    const auto row = [](std::string symbol, double position_qty) {
        MarketWatchRow result;
        result.symbol = std::move(symbol);
        result.position_qty = position_qty;
        return result;
    };
    std::vector<MarketWatchRow> rows;
    rows.push_back(row("NAN", std::numeric_limits<double>::quiet_NaN()));
    rows.push_back(row("ONE", 1.0));
    rows.push_back(row("TWO", 2.0));

    sort_market_watch(rows, MarketWatchSort::position, false);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].symbol, "ONE");
    EXPECT_EQ(rows[1].symbol, "TWO");
    EXPECT_EQ(rows[2].symbol, "NAN");

    std::rotate(rows.begin(), rows.begin() + 1, rows.end());
    sort_market_watch(rows, MarketWatchSort::position, true);
    EXPECT_EQ(rows[0].symbol, "TWO");
    EXPECT_EQ(rows[1].symbol, "ONE");
    EXPECT_EQ(rows[2].symbol, "NAN");
}

TEST(CommandCenterViewModel, ComputesPositionOrderAndProtectionDetailsOnlyWhenDefined)
{
    const auto snapshot = command_center_fixture();
    DeskState state;
    state.selected_symbol = "BTCUSDT";
    state.selected_order_id = 7;
    const auto view = build_command_center_view_model(snapshot, state);

    ASSERT_EQ(view.positions.size(), 2u);
    EXPECT_EQ(view.positions[0].symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(*view.positions[0].notional, 202.0);
    EXPECT_NEAR(*view.positions[0].unrealized_pnl_pct, 10.0 / 192.0 * 100.0, 1e-9);
    EXPECT_FALSE(view.positions[1].mark.has_value());
    EXPECT_FALSE(view.positions[1].unrealized_pnl.has_value());

    ASSERT_EQ(view.orders.size(), 2u);
    EXPECT_TRUE(view.orders[0].selected);
    EXPECT_NEAR(*view.orders[0].distance_bps, (99.0 - 101.0) / 101.0 * 1e4, 1e-9);
    EXPECT_FALSE(view.orders[1].price.has_value());
    EXPECT_FALSE(view.orders[1].distance_bps.has_value());

    ASSERT_EQ(view.protection.size(), 2u);
    EXPECT_NEAR(*view.protection[0].distance_to_stop_bps, (101.0 - 90.0) / 101.0 * 1e4, 1e-9);
    EXPECT_NEAR(*view.protection[0].distance_to_take_profit_bps, (110.0 - 101.0) / 101.0 * 1e4,
                1e-9);
    EXPECT_FALSE(view.protection[1].stop_loss.has_value());
    EXPECT_FALSE(view.protection[1].distance_to_stop_bps.has_value());
}

TEST(CommandCenterViewModel, KeepsUnavailableAndStaleStateExplicit)
{
    auto snapshot = command_center_fixture();
    snapshot.equity_available = false;
    snapshot.total_pnl_available = false;
    snapshot.generated_at_available = true;
    const auto t0 = std::chrono::steady_clock::time_point{std::chrono::seconds{1}};
    snapshot.generated_at = t0;

    const auto view = build_command_center_view_model(snapshot, {}, t0 + std::chrono::seconds{3});
    EXPECT_FALSE(view.account.equity.has_value());
    EXPECT_FALSE(view.account.total_pnl.has_value());
    ASSERT_TRUE(view.snapshot_age_ms.has_value());
    EXPECT_EQ(*view.snapshot_age_ms, 3'000);
    EXPECT_TRUE(view.snapshot_stale);
}

TEST(CommandCenterFormatting, UsesCompactTradingFormatsAndUnavailableMarker)
{
    EXPECT_EQ(format_price(70'180.25), "70180.25");
    EXPECT_EQ(format_price(0.0042), "0.00420000");
    EXPECT_EQ(format_price(std::nullopt), "—");
    EXPECT_EQ(format_money(-12.5, true), "$-12.50");
    EXPECT_EQ(format_money(12.5, true), "$+12.50");
    EXPECT_EQ(format_bps(8.25, true), "+8.2 bps");
    EXPECT_EQ(format_percent(-1.25), "-1.25%");
    EXPECT_EQ(format_quantity(0.125), "0.125000");
    EXPECT_EQ(format_duration(59), "59s");
    EXPECT_EQ(format_duration(3'600), "1h");
    EXPECT_EQ(format_duration(std::nullopt), "—");
}

TEST(CommandCenterActions, FutureTradeActionsDefaultToDisabled)
{
    const DeskTradeActions actions;
    const auto caps = derive_trade_action_capabilities(actions);
    EXPECT_FALSE(caps.close_position);
    EXPECT_FALSE(caps.change_protection);
    EXPECT_FALSE(caps.cancel_order);
    EXPECT_FALSE(caps.amend_order);
}
