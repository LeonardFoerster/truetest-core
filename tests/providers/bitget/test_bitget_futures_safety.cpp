#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_futures_safety.h"

namespace {

std::string pos_body(const char* symbol, const char* total, const char* side,
                     const char* margin, const char* mark, const char* liq)
{
    return std::string(R"({"code":"00000","data":{"list":[{)")
         + R"("symbol":")" + symbol + R"(","total":")" + total
         + R"(","posSide":")" + side + R"(","marginMode":")" + margin
         + R"(","markPrice":")" + mark + R"(","liquidationPrice":")" + liq
         + R"("}]}})";
}

} // namespace

TEST(BitgetFuturesSafety, MarginMismatchAdvisory)
{
    auto body = pos_body("BTCUSDT", "0.5", "long", "isolated", "100", "50");
    auto adv = bitget::futures::compute_advisories(
        body, "BTCUSDT", "crossed", /*liq_pct=*/0.0);
    ASSERT_EQ(adv.size(), 1u);
    EXPECT_EQ(adv[0].k, bitget::futures::advisory::kind::margin_mode_mismatch);
    EXPECT_NE(adv[0].note.find("ISOLATED"), std::string::npos);
}

TEST(BitgetFuturesSafety, LiquidationCloseAdvisory)
{
    // Long, mark=100, liq=98 → distance 2% < 5% threshold.
    auto body = pos_body("BTCUSDT", "1", "long", "crossed", "100", "98");
    auto adv = bitget::futures::compute_advisories(
        body, "BTCUSDT", /*expected_margin=*/"", /*liq_pct=*/0.05);
    ASSERT_EQ(adv.size(), 1u);
    EXPECT_EQ(adv[0].k, bitget::futures::advisory::kind::liquidation_close);
}

TEST(BitgetFuturesSafety, FlatPositionSkipped)
{
    auto body = pos_body("BTCUSDT", "0", "long", "isolated", "100", "50");
    auto adv = bitget::futures::compute_advisories(
        body, "BTCUSDT", "crossed", 0.05);
    EXPECT_TRUE(adv.empty());
}

TEST(BitgetFuturesSafety, StrictRefusalOnlyMargin)
{
    bitget::futures::advisory a;
    a.k = bitget::futures::advisory::kind::liquidation_close;
    a.note = "liq";
    bitget::futures::advisory b;
    b.k = bitget::futures::advisory::kind::margin_mode_mismatch;
    b.note = "margin bad";
    std::vector<bitget::futures::advisory> v{a, b};
    auto r = bitget::futures::first_strict_refusal(v, /*strict=*/true);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "margin bad");
    EXPECT_FALSE(bitget::futures::first_strict_refusal(v, false).has_value());
}

#endif // HAS_BITGET
