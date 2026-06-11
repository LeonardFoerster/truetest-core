#include <gtest/gtest.h>

#include "risk/futures_risk_check.h"

#include <chrono>
#include <unordered_map>

namespace {

auto now() { return std::chrono::system_clock::now(); }

order_event make_order(const std::string& symbol,
                       order_side side,
                       double qty,
                       order_type t = order_type::market)
{
    order_event o(now(), symbol, t, side, qty, /*price=*/0.0,
                  time_in_force::gtc, /*stop=*/0.0);
    o.set_order_id(1);
    return o;
}

portfolio with_state(double cash, double existing_qty,
                     const std::string& symbol = "BTCUSDT")
{
    portfolio p(0.0);
    std::unordered_map<std::string, position> positions;
    if (existing_qty != 0.0)
    {
        position pos;
        pos.qty = existing_qty;
        positions.emplace(symbol, pos);
    }
    p.restore_state(cash, /*total_trades=*/0, std::move(positions));
    return p;
}

FuturesRiskCheck::config disabled()
{
    return FuturesRiskCheck::config{};
}

}

TEST(FuturesRiskCheck, AllCapsDisabledAllows)
{
    FuturesRiskCheck check(disabled());
    auto p = with_state(/*cash=*/100.0, /*existing=*/0.0);
    auto o = make_order("BTCUSDT", order_side::buy, 1.0);

    auto d = check.evaluate(o, p, /*mark=*/30000.0);
    EXPECT_TRUE(d.allow);
    EXPECT_TRUE(d.reason.empty());
}

TEST(FuturesRiskCheck, MarkZeroSkipsAllChecks)
{
    FuturesRiskCheck::config c;
    c.max_notional_usdt = 100.0;
    FuturesRiskCheck check(c);

    auto p = with_state(100.0, 0.0);
    auto o = make_order("BTCUSDT", order_side::buy, 100.0);

    auto d = check.evaluate(o, p, /*mark=*/0.0);
    EXPECT_TRUE(d.allow) << "mark==0 must skip cleanly, not refuse";
}

TEST(FuturesRiskCheck, NotionalCapAllowsUnder)
{
    FuturesRiskCheck::config c;
    c.max_notional_usdt = 1000.0;
    FuturesRiskCheck check(c);

    auto p = with_state(100.0, 0.0);
    // 0.01 BTC * 30000 = 300 USDT < 1000 cap.
    auto o = make_order("BTCUSDT", order_side::buy, 0.01);

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_TRUE(d.allow);
}

TEST(FuturesRiskCheck, NotionalCapRejectsOver)
{
    FuturesRiskCheck::config c;
    c.max_notional_usdt = 1000.0;
    FuturesRiskCheck check(c);

    auto p = with_state(100.0, 0.0);
    // 0.05 BTC * 30000 = 1500 USDT > 1000 cap.
    auto o = make_order("BTCUSDT", order_side::buy, 0.05);

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_FALSE(d.allow);
    EXPECT_NE(d.reason.find("notional"), std::string::npos);
    EXPECT_NE(d.reason.find("1500"), std::string::npos);
}

TEST(FuturesRiskCheck, NotionalCapAddsToExistingLong)
{
    FuturesRiskCheck::config c;
    c.max_notional_usdt = 1000.0;
    FuturesRiskCheck check(c);

    // Existing 0.02 BTC long -> 600 USDT notional. Adding another 0.02
    // makes 1200 > 1000 cap. Refuse.
    auto p = with_state(100.0, 0.02);
    auto o = make_order("BTCUSDT", order_side::buy, 0.02);

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_FALSE(d.allow);
}

TEST(FuturesRiskCheck, ClosingExistingLongAllowed)
{
    FuturesRiskCheck::config c;
    c.max_notional_usdt = 100.0;  // tight cap
    FuturesRiskCheck check(c);

    // Existing 0.02 BTC long, SELL 0.02 closes to flat. post_notional = 0
    // - under any cap. Closing should always pass the notional check.
    auto p = with_state(100.0, 0.02);
    auto o = make_order("BTCUSDT", order_side::sell, 0.02);

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_TRUE(d.allow);
}

TEST(FuturesRiskCheck, FlippingFromLongToLargerShortRefused)
{
    FuturesRiskCheck::config c;
    c.max_notional_usdt = 1000.0;
    FuturesRiskCheck check(c);

    // Existing 0.01 BTC long. SELL 0.05 -> post_qty = -0.04 -> notional
    // 1200 > 1000 cap. Magnitude check works on either sign.
    auto p = with_state(100.0, 0.01);
    auto o = make_order("BTCUSDT", order_side::sell, 0.05);

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_FALSE(d.allow);
}

TEST(FuturesRiskCheck, ShortPositionBuyToCloseAllowed)
{
    FuturesRiskCheck::config c;
    c.max_notional_usdt = 100.0;  // tight cap
    FuturesRiskCheck check(c);

    // Existing 0.02 BTC short (qty = -0.02), BUY 0.02 closes to flat.
    auto p = with_state(100.0, /*existing=*/-0.02);
    auto o = make_order("BTCUSDT", order_side::buy, 0.02);

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_TRUE(d.allow);
}

TEST(FuturesRiskCheck, LeverageCapRejects)
{
    FuturesRiskCheck::config c;
    c.max_leverage = 5.0;
    FuturesRiskCheck check(c);

    // cash=100, post_notional = 0.05 BTC * 30000 = 1500 -> leverage 15x
    // > 5x cap.
    auto p = with_state(100.0, 0.0);
    auto o = make_order("BTCUSDT", order_side::buy, 0.05);

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_FALSE(d.allow);
    EXPECT_NE(d.reason.find("leverage"), std::string::npos);
}

TEST(FuturesRiskCheck, LeverageCapAllowsUnder)
{
    FuturesRiskCheck::config c;
    c.max_leverage = 5.0;
    FuturesRiskCheck check(c);

    // cash=100, post_notional = 0.01 BTC * 30000 = 300 -> leverage 3x.
    auto p = with_state(100.0, 0.0);
    auto o = make_order("BTCUSDT", order_side::buy, 0.01);

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_TRUE(d.allow);
}

TEST(FuturesRiskCheck, LeverageCapZeroCashSkipsCheck)
{
    FuturesRiskCheck::config c;
    c.max_leverage = 5.0;
    FuturesRiskCheck check(c);

    auto p = with_state(/*cash=*/0.0, 0.0);
    auto o = make_order("BTCUSDT", order_side::buy, 0.01);

    // No margin base -> can't compute leverage; skip cleanly rather
    // than divide by zero or refuse on a moot calculation.
    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_TRUE(d.allow);
}

TEST(FuturesRiskCheck, LiquidationDistanceRejectsHighLeverage)
{
    FuturesRiskCheck::config c;
    c.min_liquidation_distance_pct = 0.05;  // 5% buffer required
    c.maintenance_margin_pct       = 0.005; // 0.5%
    FuturesRiskCheck check(c);

    // cash=100, post_notional=3000 -> margin_ratio = 100/3000 = 0.033
    // distance = 0.033 - 0.005 = 0.028 = 2.8% < 5% -> refuse.
    auto p = with_state(100.0, 0.0);
    auto o = make_order("BTCUSDT", order_side::buy, 0.1);

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_FALSE(d.allow);
    EXPECT_NE(d.reason.find("liquidation"), std::string::npos);
}

TEST(FuturesRiskCheck, LiquidationDistanceAllowsLowLeverage)
{
    FuturesRiskCheck::config c;
    c.min_liquidation_distance_pct = 0.05;
    c.maintenance_margin_pct       = 0.005;
    FuturesRiskCheck check(c);

    // cash=100, post_notional=300 -> margin_ratio = 100/300 = 0.333
    // distance = 0.328 = 32.8% >> 5% -> allow.
    auto p = with_state(100.0, 0.0);
    auto o = make_order("BTCUSDT", order_side::buy, 0.01);

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_TRUE(d.allow);
}

TEST(FuturesRiskCheck, FlatPostQtySkipsLiquidationCheck)
{
    FuturesRiskCheck::config c;
    c.min_liquidation_distance_pct = 0.5;  // ridiculous buffer
    FuturesRiskCheck check(c);

    auto p = with_state(100.0, 0.02);
    // SELL exactly the existing long -> post_qty = 0 -> no exposure.
    auto o = make_order("BTCUSDT", order_side::sell, 0.02);

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_TRUE(d.allow) << "flat post_qty has no liquidation distance";
}

TEST(FuturesRiskCheck, MultipleCapsFirstRejectionWins)
{
    FuturesRiskCheck::config c;
    c.max_notional_usdt            = 1000.0;
    c.max_leverage                 = 50.0;
    c.min_liquidation_distance_pct = 0.10;
    FuturesRiskCheck check(c);

    // post_notional = 1500 > 1000 -> notional cap fires first.
    // (Implementation evaluates notional -> leverage -> liquidation,
    // and returns on first refusal.)
    auto p = with_state(100.0, 0.0);
    auto o = make_order("BTCUSDT", order_side::buy, 0.05);

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_FALSE(d.allow);
    EXPECT_NE(d.reason.find("notional"), std::string::npos);
    EXPECT_EQ(d.reason.find("leverage"), std::string::npos);
    EXPECT_EQ(d.reason.find("liquidation"), std::string::npos);
}

TEST(FuturesRiskCheck, NoopRiskCheckAlwaysAllows)
{
    NoopRiskCheck check;
    auto p = with_state(0.0, 0.0);
    auto o = make_order("BTCUSDT", order_side::buy, 1e9);

    auto d = check.evaluate(o, p, 1.0);
    EXPECT_TRUE(d.allow);
    EXPECT_TRUE(d.reason.empty());
}
