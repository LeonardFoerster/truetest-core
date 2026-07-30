#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_futures_safety.h"

namespace {

std::string pos_body(const char* symbol, const char* size, const char* side,
                     const char* trade_mode, const char* mark, const char* liq,
                     int position_idx = 0)
{
    return std::string(R"({"retCode":0,"result":{"list":[{)")
         + R"("symbol":")" + symbol + R"(","size":")" + size
         + R"(","side":")" + side + R"(","tradeMode":)" + trade_mode
         + R"(,"markPrice":")" + mark + R"(","liqPrice":")" + liq
         + R"(","positionIdx":)" + std::to_string(position_idx)
         + R"(}]}})";
}

} // namespace

TEST(BybitFuturesSafety, MarginMismatchAdvisory)
{
    // tradeMode 1 = isolated; operator expects crossed.
    auto body = pos_body("BTCUSDT", "0.5", "Buy", "1", "100", "50");
    auto adv = bybit::futures::compute_advisories(
        body, "BTCUSDT", "crossed", /*liq_pct=*/0.0);
    ASSERT_EQ(adv.size(), 1u);
    EXPECT_EQ(adv[0].k, bybit::futures::advisory::kind::margin_mode_mismatch);
    EXPECT_NE(adv[0].note.find("ISOLATED"), std::string::npos);
}

TEST(BybitFuturesSafety, LiquidationCloseAdvisory)
{
    // Long, mark=100, liq=98 → distance 2% < 5% threshold.
    auto body = pos_body("BTCUSDT", "1", "Buy", "0", "100", "98");
    auto adv = bybit::futures::compute_advisories(
        body, "BTCUSDT", /*expected_margin=*/"", /*liq_pct=*/0.05);
    ASSERT_EQ(adv.size(), 1u);
    EXPECT_EQ(adv[0].k, bybit::futures::advisory::kind::liquidation_close);
}

TEST(BybitFuturesSafety, FlatPositionSkipped)
{
    auto body = pos_body("BTCUSDT", "0", "Buy", "1", "100", "50");
    auto adv = bybit::futures::compute_advisories(
        body, "BTCUSDT", "crossed", 0.05);
    EXPECT_TRUE(adv.empty());
}

TEST(BybitFuturesSafety, StrictRefusalOnlyMargin)
{
    bybit::futures::advisory a;
    a.k = bybit::futures::advisory::kind::liquidation_close;
    a.note = "liq";
    bybit::futures::advisory b;
    b.k = bybit::futures::advisory::kind::margin_mode_mismatch;
    b.note = "margin bad";
    std::vector<bybit::futures::advisory> v{a, b};
    auto r = bybit::futures::first_strict_refusal(v, /*strict=*/true);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "margin bad");
    EXPECT_FALSE(bybit::futures::first_strict_refusal(v, false).has_value());
}

TEST(BybitFuturesSafety, HedgeModeDetectedByPositionIdx)
{
    auto body = pos_body("BTCUSDT", "0.1", "Buy", "0", "100", "50", /*idx=*/1);
    auto err = bybit::futures::check_one_way_position_mode(body, "BTCUSDT");
    EXPECT_NE(err.find("hedge"), std::string::npos);
    EXPECT_NE(err.find("positionIdx"), std::string::npos);
}

TEST(BybitFuturesSafety, OneWayPositionIdxZeroOk)
{
    auto body = pos_body("BTCUSDT", "0.1", "Buy", "0", "100", "50", /*idx=*/0);
    auto err = bybit::futures::check_one_way_position_mode(body, "BTCUSDT");
    EXPECT_TRUE(err.empty());
}

TEST(BybitFuturesSafety, EmptyPositionListOk)
{
    auto err = bybit::futures::check_one_way_position_mode(
        R"({"retCode":0,"result":{"list":[]}})", "BTCUSDT");
    EXPECT_TRUE(err.empty());
}

#endif // HAS_BYBIT
