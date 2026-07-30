#include <gtest/gtest.h>
#include "analytics/analytics.h"
#include "risk/risk_manager.h"

static auto epoch_ms(int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

// Helper: build an AnalyticsReport with specific fields
static AnalyticsReport make_snap(double max_dd_pct = 0.0,
                                 std::size_t total_orders = 0,
                                 std::size_t total_fills = 0)
{
    AnalyticsReport r;
    r.max_drawdown = max_dd_pct;  // already in percent (e.g. 25.0 = 25%)
    r.total_orders = total_orders;
    r.total_fills = total_fills;
    return r;
}

TEST(RiskManager, OrderWithinLimits_Pass)
{
    risk_limits lim;
    lim.max_position_value = 1e6;
    lim.max_drawdown = 0.30;
    lim.max_open_orders = 100;
    lim.max_portfolio_exposure = 5e6;

    RiskManager rm(lim);
    portfolio port;

    order_event ord(epoch_ms(0), "AAPL", order_type::limit, order_side::buy, 10, 150.0);
    auto snap = make_snap(0.0, 5, 5);

    EXPECT_EQ(rm.check_order(ord, port, snap), risk_action::pass);
}

TEST(RiskManager, MaxPositionValue_Reject)
{
    risk_limits lim;
    lim.max_position_value = 5000.0;  // very tight

    RiskManager rm(lim);
    portfolio port;

    // Simulate existing position by filling a buy
    fill_event fill(epoch_ms(0), "AAPL", 1, order_side::buy, 40, 100.0, 0.0);
    port.on_fill(fill);
    // Now position: 40 shares at 100 = $4000 notional

    // Try to buy 20 more at 100 = $2000 additional -> total $6000 > $5000 limit
    order_event ord(epoch_ms(1), "AAPL", order_type::limit, order_side::buy, 20, 100.0);
    auto snap = make_snap(0.0, 1, 1);

    EXPECT_EQ(rm.check_order(ord, port, snap), risk_action::reject);
}

TEST(RiskManager, MaxPositionValue_AllowsLongReductionNearLimit)
{
    risk_limits lim;
    lim.max_position_value = 5000.0;
    lim.max_portfolio_exposure = 5000.0;

    RiskManager rm(lim);
    portfolio port;

    fill_event fill(epoch_ms(0), "AAPL", 1, order_side::buy, 40, 100.0, 0.0);
    port.on_fill(fill);

    order_event reduce(epoch_ms(1), "AAPL", order_type::limit, order_side::sell, 10, 100.0);
    auto snap = make_snap(0.0, 1, 1);

    EXPECT_EQ(rm.check_order(reduce, port, snap), risk_action::pass);
}

TEST(RiskManager, MaxPositionValue_RejectsShortIncrease)
{
    risk_limits lim;
    lim.max_position_value = 5000.0;
    lim.max_portfolio_exposure = 5000.0;

    RiskManager rm(lim);
    portfolio port;

    order_event short_open(epoch_ms(1), "AAPL", order_type::limit, order_side::sell, 60, 100.0);
    auto snap = make_snap(0.0, 0, 0);

    EXPECT_EQ(rm.check_order(short_open, port, snap), risk_action::reject);
}

TEST(RiskManager, MaxPositionValue_AllowsShortReductionNearLimit)
{
    risk_limits lim;
    lim.max_position_value = 5000.0;
    lim.max_portfolio_exposure = 5000.0;

    RiskManager rm(lim);
    portfolio port;

    fill_event fill(epoch_ms(0), "AAPL", 1, order_side::sell, 40, 100.0, 0.0);
    port.on_fill(fill);

    order_event reduce(epoch_ms(1), "AAPL", order_type::limit, order_side::buy, 10, 100.0);
    auto snap = make_snap(0.0, 1, 1);

    EXPECT_EQ(rm.check_order(reduce, port, snap), risk_action::pass);
}

TEST(RiskManager, DrawdownExceeded_Halt)
{
    risk_limits lim;
    lim.max_drawdown = 0.10;  // 10% max drawdown

    RiskManager rm(lim);
    portfolio port;

    order_event ord(epoch_ms(0), "AAPL", order_type::limit, order_side::buy, 10, 100.0);
    // Snap shows 15% drawdown (in percent) -> 0.15 >= 0.10 limit
    auto snap = make_snap(15.0, 0, 0);

    EXPECT_EQ(rm.check_order(ord, port, snap), risk_action::halt);
}

TEST(RiskManager, PostFillLossExceeded_Halt)
{
    risk_limits lim;
    lim.max_loss_per_trade = 500.0;

    RiskManager rm(lim);
    portfolio port;

    fill_event fill(epoch_ms(0), "AAPL", 1, order_side::sell, 10, 90.0, 0.0);

    // Build a report with a losing trade in the trade log
    AnalyticsReport snap;
    trade_record rec;
    rec.order_id = 1;
    rec.side = order_side::sell;
    rec.quantity = 10;
    rec.fill_price = 90.0;
    rec.commission = 0.0;
    rec.intended_price = 90.0;
    rec.timestamp = epoch_ms(0);
    rec.pnl = -1000.0;  // exceeds $500 limit
    snap.trades.push_back(rec);
    snap.max_drawdown = 0.0;

    EXPECT_EQ(rm.check_post_fill(fill, port, snap), risk_action::halt);
}

TEST(RiskManager, DailyLossCountsEqualPnLLossesAsSeparateTrades)
{
    risk_limits lim;
    lim.max_daily_loss = 150.0;

    RiskManager rm(lim);
    portfolio port;

    fill_event fill1(epoch_ms(0), "AAPL", 1, order_side::sell, 10, 90.0, 0.0);
    risk_snapshot snap1;
    snap1.has_last_trade = true;
    snap1.last_trade_pnl = -100.0;
    snap1.last_trade_seq = 1;

    EXPECT_EQ(rm.check_post_fill(fill1, port, snap1), risk_action::pass);

    fill_event fill2(epoch_ms(1), "AAPL", 2, order_side::sell, 10, 90.0, 0.0);
    risk_snapshot snap2;
    snap2.has_last_trade = true;
    snap2.last_trade_pnl = -100.0;
    snap2.last_trade_seq = 2;

    EXPECT_EQ(rm.check_post_fill(fill2, port, snap2), risk_action::halt);
}

TEST(RiskManager, ZeroPriceMarketOpen_RejectsFailClosed)
{
    risk_limits lim;
    lim.max_position_value = 1000.0;

    RiskManager rm(lim);
    portfolio port;

    // Market open with price=0 and no avg-entry fallback would previously
    // project notional 0 and bypass max_position_value.
    order_event mkt(epoch_ms(0), "AAPL", order_type::market, order_side::buy, 100, 0.0);
    risk_snapshot snap;
    snap.equity = 100000.0;

    EXPECT_EQ(rm.check_order(mkt, port, snap), risk_action::reject);
}

TEST(RiskManager, MaxOpenOrders_UsesActiveCountNotOrdersMinusFills)
{
    risk_limits lim;
    lim.max_open_orders = 2;

    RiskManager rm(lim);
    portfolio port;

    order_event ord(epoch_ms(0), "AAPL", order_type::limit, order_side::buy, 1, 100.0);

    // Legacy proxy would see 10-8=2 and reject; true active is 1 → pass.
    risk_snapshot snap;
    snap.total_orders = 10;
    snap.total_fills = 8;
    snap.active_orders = 1;
    snap.active_orders_valid = true;
    EXPECT_EQ(rm.check_order(ord, port, snap), risk_action::pass);

    snap.active_orders = 2;
    EXPECT_EQ(rm.check_order(ord, port, snap), risk_action::reject);

    // Partial-fill undercount: fills > "open proxy" would underflow; active wins.
    risk_snapshot partial;
    partial.total_orders = 3;
    partial.total_fills = 5;  // multi-partial fills
    partial.active_orders = 2;
    partial.active_orders_valid = true;
    EXPECT_EQ(rm.check_order(ord, port, partial), risk_action::reject);
}

TEST(RiskManager, MaxPositionPctOfEquity_RejectsWhenEquityPopulated)
{
    risk_limits lim;
    lim.max_position_value = 1e12;  // absolute cap not binding
    lim.max_position_pct_of_equity = 0.10;  // 10% of equity

    RiskManager rm(lim);
    portfolio port;

    risk_snapshot snap;
    snap.equity = 10000.0;  // max notional = 1000

    order_event small(epoch_ms(0), "AAPL", order_type::limit, order_side::buy, 5, 100.0);
    EXPECT_EQ(rm.check_order(small, port, snap), risk_action::pass);

    order_event large(epoch_ms(1), "AAPL", order_type::limit, order_side::buy, 20, 100.0);
    EXPECT_EQ(rm.check_order(large, port, snap), risk_action::reject);
}

TEST(RiskManager, MaxOpenOrders_Reject)
{
    risk_limits lim;
    lim.max_open_orders = 5;

    RiskManager rm(lim);
    portfolio port;

    order_event ord(epoch_ms(0), "AAPL", order_type::limit, order_side::buy, 10, 100.0);
    // 10 orders sent, 5 filled -> 5 open = at limit
    auto snap = make_snap(0.0, 10, 5);

    EXPECT_EQ(rm.check_order(ord, port, snap), risk_action::reject);
}
