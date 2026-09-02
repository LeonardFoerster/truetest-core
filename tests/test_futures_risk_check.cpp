#include <gtest/gtest.h>

#include "risk/futures_risk_check.h"

#include <chrono>
#include <limits>
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

portfolio with_equity_and_position(double equity, double existing_qty,
                                   double mark_price,
                                   const std::string& symbol = "BTCUSDT")
{
    // Portfolio uses spot-style cash settlement internally.  Choose cash so
    // cash + signed position value equals the requested account equity for
    // both long and short fixtures.
    return with_state(equity - existing_qty * mark_price,
                      existing_qty, symbol);
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

TEST(FuturesRiskCheck, InvalidMarkRejectsNewExposureFailClosed)
{
    FuturesRiskCheck::config c;
    c.max_notional_usdt = 100.0;
    FuturesRiskCheck check(c);

    auto p = with_state(100.0, 0.0);
    auto o = make_order("BTCUSDT", order_side::buy, 100.0);

    for (const double mark : {
             0.0, -1.0, std::numeric_limits<double>::quiet_NaN()})
    {
        auto d = check.evaluate(o, p, mark);
        EXPECT_FALSE(d.allow) << "mark=" << mark;
        EXPECT_NE(d.reason.find("invalid mark"), std::string::npos)
            << d.reason;
    }
}

TEST(FuturesRiskCheck, InvalidMarkStillAllowsStrictReduction)
{
    FuturesRiskCheck::config c;
    c.max_notional_usdt = 100.0;
    FuturesRiskCheck check(c);

    auto long_port = with_state(100.0, 10.0);
    auto short_port = with_state(100.0, -10.0);
    for (const double mark : {
             0.0, -1.0, std::numeric_limits<double>::quiet_NaN()})
    {
        EXPECT_TRUE(check.evaluate(
            make_order("BTCUSDT", order_side::sell, 5.0),
            long_port, mark).allow) << "long mark=" << mark;
        EXPECT_TRUE(check.evaluate(
            make_order("BTCUSDT", order_side::buy, 5.0),
            short_port, mark).allow) << "short mark=" << mark;
    }
}

TEST(FuturesRiskCheck, InvalidOrderQuantityRejectsFailClosed)
{
    FuturesRiskCheck::config c;
    c.max_leverage = 5.0;
    FuturesRiskCheck check(c);
    auto p = with_state(100.0, 0.0);

    for (const double qty : {
             0.0, -1.0, std::numeric_limits<double>::quiet_NaN()})
    {
        auto d = check.evaluate(
            make_order("BTCUSDT", order_side::buy, qty), p, 100.0);
        EXPECT_FALSE(d.allow) << "qty=" << qty;
        EXPECT_NE(d.reason.find("quantity"), std::string::npos);
    }
}

TEST(FuturesRiskCheck, NonFiniteExistingOrPostTradePositionRejectsFailClosed)
{
    FuturesRiskCheck::config c;
    c.max_leverage = 5.0;
    FuturesRiskCheck check(c);

    auto poisoned = with_state(
        100.0, std::numeric_limits<double>::quiet_NaN());
    auto poisoned_decision = check.evaluate(
        make_order("BTCUSDT", order_side::sell, 1.0), poisoned, 100.0);
    EXPECT_FALSE(poisoned_decision.allow);
    EXPECT_NE(poisoned_decision.reason.find("existing position"),
              std::string::npos);

    auto extreme = with_state(100.0, std::numeric_limits<double>::max());
    auto overflow_decision = check.evaluate(
        make_order("BTCUSDT", order_side::buy,
                   std::numeric_limits<double>::max()),
        extreme, 100.0);
    EXPECT_FALSE(overflow_decision.allow);
    EXPECT_NE(overflow_decision.reason.find("post-trade position"),
              std::string::npos);
}

TEST(FuturesRiskCheck, NonFiniteConfiguredCapFailsClosedForNewRisk)
{
    FuturesRiskCheck::config cfg;
    cfg.max_leverage = std::numeric_limits<double>::infinity();
    FuturesRiskCheck check(cfg);
    portfolio p(1000.0);

    const auto d = check.evaluate(
        make_order("BTCUSDT", order_side::buy, 1.0), p, 100.0);
    EXPECT_FALSE(d.allow);
    EXPECT_NE(d.reason.find("risk configuration"), std::string::npos);
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

    // equity=100, post_notional = 0.05 BTC * 30000 = 1500 -> leverage 15x
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

    // equity=100, post_notional = 0.01 BTC * 30000 = 300 -> leverage 3x.
    auto p = with_state(100.0, 0.0);
    auto o = make_order("BTCUSDT", order_side::buy, 0.01);

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_TRUE(d.allow);
}

TEST(FuturesRiskCheck, LeverageCapUsesEquitySymmetricallyForLongAndShort)
{
    FuturesRiskCheck::config c;
    c.max_leverage = 1.5;
    FuturesRiskCheck check(c);

    constexpr double mark = 30000.0;
    // Both portfolios have 1000 equity and 0.03 BTC existing exposure.
    // Adding another 0.03 makes post-notional 1800, hence 1.8x leverage.
    // Their spot-style cash balances differ (long=100, short=1900), so a
    // cash denominator would incorrectly reject only the long.
    auto long_port = with_equity_and_position(1000.0, 0.03, mark);
    auto short_port = with_equity_and_position(1000.0, -0.03, mark);
    auto add_long = make_order("BTCUSDT", order_side::buy, 0.03);
    auto add_short = make_order("BTCUSDT", order_side::sell, 0.03);

    auto long_d = check.evaluate(add_long, long_port, mark);
    auto short_d = check.evaluate(add_short, short_port, mark);

    EXPECT_FALSE(long_d.allow);
    EXPECT_FALSE(short_d.allow);
    EXPECT_NE(long_d.reason.find("leverage"), std::string::npos);
    EXPECT_NE(short_d.reason.find("leverage"), std::string::npos);
}

TEST(FuturesRiskCheck, RiskIncreaseWithNonPositiveEquityRejectsFailClosed)
{
    FuturesRiskCheck::config c;
    c.max_leverage = 5.0;
    FuturesRiskCheck check(c);

    for (double equity : {0.0, -100.0})
    {
        auto p = with_equity_and_position(
            equity, /*existing_qty=*/0.0, /*mark=*/30000.0);
        auto o = make_order("BTCUSDT", order_side::buy, 0.01);

        auto d = check.evaluate(o, p, 30000.0);
        EXPECT_FALSE(d.allow) << "equity=" << equity;
        EXPECT_NE(d.reason.find("non-positive equity"), std::string::npos)
            << "equity=" << equity << " reason=" << d.reason;
    }
}

TEST(FuturesRiskCheck, LiquidationDistanceRejectsHighLeverage)
{
    FuturesRiskCheck::config c;
    c.min_liquidation_distance_pct = 0.05;  // 5% buffer required
    c.maintenance_margin_pct       = 0.005; // 0.5%
    FuturesRiskCheck check(c);

    // equity=100, post_notional=3000 -> margin_ratio = 100/3000 = 0.033
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

    // equity=100, post_notional=300 -> margin_ratio = 100/300 = 0.333
    // distance = 0.328 = 32.8% >> 5% -> allow.
    auto p = with_state(100.0, 0.0);
    auto o = make_order("BTCUSDT", order_side::buy, 0.01);

    auto d = check.evaluate(o, p, 30000.0);
    EXPECT_TRUE(d.allow);
}

TEST(FuturesRiskCheck, LiquidationDistanceUsesEquitySymmetricallyForLongAndShort)
{
    FuturesRiskCheck::config c;
    c.min_liquidation_distance_pct = 0.60;
    c.maintenance_margin_pct = 0.005;
    FuturesRiskCheck check(c);

    constexpr double mark = 30000.0;
    // Equal 1000 equity and equal post-notional 1800 produce the same
    // projected distance: 1000/1800 - 0.005 ~= 55.06%, below 60%.
    // A cash denominator would see long cash=100 and short cash=1900.
    auto long_port = with_equity_and_position(1000.0, 0.03, mark);
    auto short_port = with_equity_and_position(1000.0, -0.03, mark);
    auto add_long = make_order("BTCUSDT", order_side::buy, 0.03);
    auto add_short = make_order("BTCUSDT", order_side::sell, 0.03);

    auto long_d = check.evaluate(add_long, long_port, mark);
    auto short_d = check.evaluate(add_short, short_port, mark);

    EXPECT_FALSE(long_d.allow);
    EXPECT_FALSE(short_d.allow);
    EXPECT_NE(long_d.reason.find("liquidation"), std::string::npos);
    EXPECT_NE(short_d.reason.find("liquidation"), std::string::npos);
}

TEST(FuturesRiskCheck, EngineSuppliedMultiSymbolEquityAvoidsSingleMarkDistortion)
{
    FuturesRiskCheck::config c;
    c.max_leverage = 1.0;
    FuturesRiskCheck check(c);

    portfolio p(1'000.0);
    const auto ts = now();
    fill_event btc(ts, "BTCUSDT", 10, order_side::buy,
                   1.0, 100.0, 0.0);
    fill_event eth(ts, "ETHUSDT", 20, order_side::buy,
                   100.0, 10.0, 0.0);
    p.on_fill(btc, 10, "alpha");
    p.on_fill(eth, 20, "alpha");

    auto add_btc = make_order("BTCUSDT", order_side::buy, 10.0);

    // The legacy compatibility overload marks both positions at BTC=100,
    // inventing 10,000 of ETH value and therefore allowing 0.11x leverage.
    EXPECT_TRUE(check.evaluate(add_btc, p, 100.0).allow);

    // Engine context marks BTC at 100 and ETH at 10: true account equity is
    // 1,000 and post-BTC notional is 1,100, so the 1x cap must reject.
    auto exact = check.evaluate_with_account_equity(
        add_btc, p, 100.0, /*account_equity=*/1'000.0);
    EXPECT_FALSE(exact.allow);
    EXPECT_NE(exact.reason.find("leverage"), std::string::npos);

    // Missing marks for any other open symbol are represented as non-finite
    // equity and must fail closed for risk-increasing orders.
    auto missing = check.evaluate_with_account_equity(
        add_btc, p, 100.0,
        std::numeric_limits<double>::quiet_NaN());
    EXPECT_FALSE(missing.allow);
    EXPECT_NE(missing.reason.find("non-positive equity"), std::string::npos);
}

TEST(FuturesRiskCheck, BadEquityAndCapsStillAllowStrictLongAndShortReduction)
{
    FuturesRiskCheck::config c;
    c.max_notional_usdt = 10.0;
    c.max_leverage = 0.1;
    c.min_liquidation_distance_pct = 0.90;
    FuturesRiskCheck check(c);

    constexpr double mark = 100.0;
    auto long_port = with_equity_and_position(-50.0, 10.0, mark);
    auto short_port = with_equity_and_position(-50.0, -10.0, mark);
    auto reduce_long = make_order("BTCUSDT", order_side::sell, 1.0);
    auto reduce_short = make_order("BTCUSDT", order_side::buy, 1.0);

    // Each order leaves 900 notional, still far above every configured cap,
    // but neither can increase or reverse exposure.
    EXPECT_TRUE(check.evaluate(reduce_long, long_port, mark).allow);
    EXPECT_TRUE(check.evaluate(reduce_short, short_port, mark).allow);
}

TEST(FuturesRiskCheck, BadEquityAndCapsStillAllowLongAndShortFlatten)
{
    FuturesRiskCheck::config c;
    c.max_notional_usdt = 10.0;
    c.max_leverage = 0.1;
    c.min_liquidation_distance_pct = 0.90;
    FuturesRiskCheck check(c);

    constexpr double mark = 100.0;
    auto long_port = with_equity_and_position(-50.0, 10.0, mark);
    auto short_port = with_equity_and_position(-50.0, -10.0, mark);
    auto flatten_long = make_order("BTCUSDT", order_side::sell, 10.0);
    auto flatten_short = make_order("BTCUSDT", order_side::buy, 10.0);

    EXPECT_TRUE(check.evaluate(flatten_long, long_port, mark).allow);
    EXPECT_TRUE(check.evaluate(flatten_short, short_port, mark).allow);
}

TEST(FuturesRiskCheck, CrossingFlatIsNotClassifiedAsExposureReduction)
{
    FuturesRiskCheck::config c;
    c.max_leverage = 5.0;
    FuturesRiskCheck check(c);

    constexpr double mark = 100.0;
    auto p = with_equity_and_position(-50.0, 10.0, mark);
    // SELL 15 closes the long and opens a short 5.  Although the final
    // absolute exposure is smaller, the order creates opposite-side risk.
    auto flip = make_order("BTCUSDT", order_side::sell, 15.0);

    auto d = check.evaluate(flip, p, mark);
    EXPECT_FALSE(d.allow);
    EXPECT_NE(d.reason.find("non-positive equity"), std::string::npos);
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
    ASSERT_TRUE(table.load_from_leverage_bracket_json(R"json(
        {
          "symbol": "BTCUSDT",
          "brackets": [
            {"bracket": 2, "notionalFloor": 10000, "notionalCap": 50000, "maintMarginRatio": 0.005, "cum": 100},
            {"bracket": 1, "notionalFloor": 0, "notionalCap": 10000, "maintMarginRatio": 0.004, "cum": 0}
          ]
        }
    )json"));

    EXPECT_FALSE(table.empty());
    EXPECT_DOUBLE_EQ(table.maintenance_margin_rate_for_notional(9000.0), 0.004);
    EXPECT_DOUBLE_EQ(table.maint_amount_for_notional(9000.0), 0.0);
    EXPECT_DOUBLE_EQ(table.maintenance_margin_rate_for_notional(20000.0), 0.005);
    EXPECT_DOUBLE_EQ(table.maint_amount_for_notional(20000.0), 100.0);
}

TEST(MaintenanceMarginTable, ParsesArrayLeverageBracketPayloadAndNumericStrings)
{
    MaintenanceMarginTable table;
    ASSERT_TRUE(table.load_from_leverage_bracket_json(R"json(
        [
          {
            "symbol": "ETHUSDT",
            "brackets": [
              {"notionalFloor": "0", "notionalCap": "10000", "maintMarginRatio": "0.0065", "cum": "0"},
              {"notionalFloor": "10000", "notionalCap": "50000", "maintMarginRatio": "0.01", "cum": "35"}
            ]
          }
        ]
    )json"));

    EXPECT_FALSE(table.empty());
    EXPECT_DOUBLE_EQ(table.maintenance_margin_rate_for_notional(10000.0), 0.0065);
    EXPECT_DOUBLE_EQ(table.maintenance_margin_rate_for_notional(10001.0), 0.01);
    EXPECT_DOUBLE_EQ(table.maint_amount_for_notional(10001.0), 35.0);
}

TEST(MaintenanceMarginTable, InvalidPayloadLeavesTableEmpty)
{
    MaintenanceMarginTable table;
    EXPECT_FALSE(table.load_from_leverage_bracket_json(
        R"json({"symbol":"BTCUSDT","no_brackets":[]})json"));

    EXPECT_TRUE(table.empty());
    EXPECT_FALSE(table.valid());
    EXPECT_DOUBLE_EQ(table.maintenance_margin_rate_for_notional(10000.0), 1.0);
    EXPECT_TRUE(std::isinf(table.maint_amount_for_notional(10000.0)));
}

TEST(MaintenanceMarginTable, RejectsMalformedOrUnsafeTierAtomically)
{
    const std::vector<std::string> invalid_payloads = {
        R"json({"symbol":"BTCUSDT","brackets":[{"notionalFloor":0,"notionalCap":10000,"cum":0}]})json",
        R"json({"symbol":"BTCUSDT","brackets":[{"notionalFloor":0,"notionalCap":10000,"maintMarginRatio":"0.004junk","cum":0}]})json",
        R"json({"symbol":"BTCUSDT","brackets":[{"notionalFloor":0,"notionalCap":10000,"maintMarginRatio":"NaN","cum":0}]})json",
        R"json({"symbol":"BTCUSDT","brackets":[{"notionalFloor":0,"notionalCap":0,"maintMarginRatio":0.004,"cum":0}]})json",
        R"json({"symbol":"BTCUSDT","brackets":[{"notionalFloor":0,"notionalCap":10000,"maintMarginRatio":-0.004,"cum":0}]})json",
        R"json({"symbol":"BTCUSDT","brackets":[{"notionalFloor":0,"notionalCap":10000,"maintMarginRatio":0.004,"cum":-1}]})json",
        R"json({"symbol":"BTCUSDT","brackets":[{"note":"x \"notionalFloor\":0, \"notionalCap\":10000, \"maintMarginRatio\":0.004, \"cum\":0"}]})json",
        R"json({"symbol":"BTCUSDT","brackets":[{"notionalFloor":0,"notionalFloor":0,"notionalCap":10000,"maintMarginRatio":0.004,"cum":0}]})json",
        R"json({"symbol":"BTCUSDT","brackets":[{"notionalFloor":0,"notionalCap":10000,"maintMarginRatio":{},"cum":0}]})json",
        R"json({"symbol":"BTCUSDT","brackets":[{"notionalFloor":0,"notionalCap":10000,"maintMarginRatio":0.01,"cum":0},{"notionalFloor":10000,"notionalCap":20000,"maintMarginRatio":0.005,"cum":1}]})json",
        R"json({"symbol":"BTCUSDT","brackets":[{"notionalFloor":0,"notionalCap":10000,"maintMarginRatio":0.004,"cum":0},{"notionalFloor":10000,"notionalCap":10000,"maintMarginRatio":0.005,"cum":1}]})json",
        R"json({"symbol":"BTCUSDT","brackets":[{"notionalFloor":0,"notionalCap":10000,"maintMarginRatio":0.004,"cum":0},{"notionalFloor":9000,"notionalCap":20000,"maintMarginRatio":0.005,"cum":1}]})json"
    };

    for (const auto& payload : invalid_payloads) {
        MaintenanceMarginTable table;
        EXPECT_FALSE(table.load_from_leverage_bracket_json(payload)) << payload;
        EXPECT_TRUE(table.empty()) << payload;
        EXPECT_FALSE(table.valid()) << payload;
    }
}

TEST(MaintenanceMarginTable, RejectsResponseForDifferentSymbol)
{
    MaintenanceMarginTable table;
    EXPECT_FALSE(table.load_from_leverage_bracket_json(
        R"json({"symbol":"ETHUSDT","brackets":[{"notionalFloor":0,"notionalCap":10000,"maintMarginRatio":0.004,"cum":0}]})json",
        "BTCUSDT"));
    EXPECT_FALSE(table.valid());
}

TEST(MaintenanceMarginTable, RejectsNonAuthoritativeJsonStructure)
{
    const std::string tier =
        R"json({"notionalFloor":0,"notionalCap":10000,"maintMarginRatio":0.004,"cum":0})json";
    const std::vector<std::string> invalid_payloads = {
        std::string{"{\"nested\":{\"symbol\":\"BTCUSDT\",\"brackets\":["}
            + tier + "]}}",
        std::string{"{\"symbol\":\"BTCUSDT\",\"brackets\":["}
            + tier + "]} trailing",
        std::string{"{\"symbol\":\"BTCUSDT\",\v\"brackets\":["}
            + tier + "]}",
        std::string{"{\"symbol\":\"BTCUSDT\",\f\"brackets\":["}
            + tier + "]}",
        std::string{"{\"symbol\":\"BTCUSDT\",\"brackets\" junk :["}
            + tier + "]}",
        std::string{"{\"symbol\":\"BTCUSDT\",\"brackets\":["}
            + tier + "}] }"
    };

    for (const auto& payload : invalid_payloads) {
        MaintenanceMarginTable table;
        EXPECT_FALSE(table.load_from_leverage_bracket_json(payload)) << payload;
        EXPECT_FALSE(table.valid()) << payload;
        EXPECT_TRUE(table.empty()) << payload;
    }
}
