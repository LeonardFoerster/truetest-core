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

// Spot-style cash credits short proceeds. Leverage must use mark equity
// (cash + qty*mark), not inflated cash — otherwise max_leverage fails open.
TEST(FuturesRiskCheck, AfterShort_LeverageUsesMarkEquityNotCash)
{
    FuturesRiskCheck::config c;
    c.max_leverage = 5.0;
    FuturesRiskCheck check(c);

    // Post short 1 BTC @ 30k from 10k equity: cash=40k, qty=-1.
    // Equity = 40k + (-1)*30k = 10k. Add another 1 BTC short:
    // post_notional=60k → lev 6x on equity (reject) vs 1.5x on cash (would allow).
    auto p = with_state(/*cash=*/40000.0, /*existing=*/-1.0);
    auto o = make_order("BTCUSDT", order_side::sell, 1.0);

    auto d = check.evaluate(o, p, /*mark=*/30000.0);
    EXPECT_FALSE(d.allow) << d.reason;
    EXPECT_NE(d.reason.find("leverage"), std::string::npos);
    EXPECT_NE(d.reason.find("equity"), std::string::npos);
}

TEST(FuturesRiskCheck, AfterShort_LiquidationUsesMarkEquityNotCash)
{
    FuturesRiskCheck::config c;
    c.min_liquidation_distance_pct = 0.05;  // 5%
    c.maintenance_margin_pct       = 0.005;
    FuturesRiskCheck check(c);

    // cash=40k, short 1 @ 30k → equity 10k. Add short 1 → notional 60k.
    // equity margin_ratio = 10k/60k ≈ 0.167, distance ≈ 16.2% > 5% → allow.
    // cash margin_ratio = 40k/60k would be even looser; tighten by stacking.
    auto p = with_state(40000.0, -1.0);
    // Add 2 BTC short: post_qty=-3, notional=90k, equity still 10k
    // ratio=10k/90k≈0.111, distance≈10.6% > 5% → still allow
    // Add enough that distance fails on equity but would pass on cash:
    // post notional with equity 10k needs distance < 5% → ratio < 0.055
    // → notional > 10k/0.055 ≈ 181818 → qty add ≈ 181818/30000 - 1 ≈ 5.06
    auto o = make_order("BTCUSDT", order_side::sell, 6.0); // post_qty=-7, notional=210k
    // equity ratio = 10k/210k ≈ 0.0476 - 0.005 = 0.0426 < 5% → reject
    // cash ratio = 40k/210k ≈ 0.190 - 0.005 = 0.185 >> 5% → would allow

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_FALSE(d.allow) << d.reason;
    EXPECT_NE(d.reason.find("liquidation"), std::string::npos);
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

TEST(MaintenanceMarginTable, ParsesSymbolSpecificLeverageBracketPayload)
{
    MaintenanceMarginTable table;
    table.load_from_leverage_bracket_json(R"json(
        {
          "symbol": "BTCUSDT",
          "brackets": [
            {"bracket": 2, "notionalCap": 50000, "maintMarginRatio": 0.005, "cum": 100},
            {"bracket": 1, "notionalCap": 10000, "maintMarginRatio": 0.004, "cum": 0}
          ]
        }
    )json");

    EXPECT_FALSE(table.empty());
    EXPECT_DOUBLE_EQ(table.maintenance_margin_rate_for_notional(9000.0), 0.004);
    EXPECT_DOUBLE_EQ(table.maint_amount_for_notional(9000.0), 0.0);
    EXPECT_DOUBLE_EQ(table.maintenance_margin_rate_for_notional(20000.0), 0.005);
    EXPECT_DOUBLE_EQ(table.maint_amount_for_notional(20000.0), 100.0);
}

TEST(MaintenanceMarginTable, ParsesArrayLeverageBracketPayloadAndNumericStrings)
{
    MaintenanceMarginTable table;
    table.load_from_leverage_bracket_json(R"json(
        [
          {
            "symbol": "ETHUSDT",
            "brackets": [
              {"notionalCap": "10000", "maintMarginRatio": "0.0065", "cum": "0"},
              {"notionalCap": "50000", "maintMarginRatio": "0.01", "cum": "35"}
            ]
          }
        ]
    )json");

    EXPECT_FALSE(table.empty());
    EXPECT_DOUBLE_EQ(table.maintenance_margin_rate_for_notional(10000.0), 0.0065);
    EXPECT_DOUBLE_EQ(table.maintenance_margin_rate_for_notional(10001.0), 0.01);
    EXPECT_DOUBLE_EQ(table.maint_amount_for_notional(10001.0), 35.0);
}

TEST(MaintenanceMarginTable, InvalidPayloadLeavesTableEmpty)
{
    MaintenanceMarginTable table;
    table.load_from_leverage_bracket_json(R"json({"symbol":"BTCUSDT","no_brackets":[]})json");

    EXPECT_TRUE(table.empty());
    EXPECT_DOUBLE_EQ(table.maintenance_margin_rate_for_notional(10000.0), 0.005);
    EXPECT_DOUBLE_EQ(table.maint_amount_for_notional(10000.0), 0.0);
}
