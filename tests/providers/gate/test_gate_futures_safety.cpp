#include <gtest/gtest.h>

#ifdef HAS_GATE

#include "providers/gate/gate_futures_safety.h"

namespace {

std::string accounts(const char* dual, const char* mode = "single")
{
    return std::string(R"({"user":1,"currency":"USDT","available":"1000",)"
                       R"("in_dual_mode":)")
         + dual + R"(,"position_mode":")" + mode + R"("})";
}

std::string pos_body(const char* contract, const char* size,
                     const char* margin, const char* mark, const char* liq)
{
    return std::string(R"({"contract":")") + contract + R"(","size":)" + size
         + R"(,"margin_mode":")" + margin + R"(","mark_price":")" + mark
         + R"(","liq_price":")" + liq + R"("})";
}

} // namespace

TEST(GateFuturesSafety, DualModeTrueRefuses)
{
    auto note = gate::futures::dual_mode_refusal(accounts("true"));
    ASSERT_TRUE(note.has_value());
    EXPECT_NE(note->find("in_dual_mode"), std::string::npos);
}

TEST(GateFuturesSafety, DualModeFalseAllows)
{
    auto note = gate::futures::dual_mode_refusal(accounts("false"));
    EXPECT_FALSE(note.has_value());
}

TEST(GateFuturesSafety, DualModeBooleanLiteral)
{
    std::string body =
        R"({"available":"10","in_dual_mode":true,"position_mode":"single"})";
    auto note = gate::futures::dual_mode_refusal(body);
    ASSERT_TRUE(note.has_value());
    EXPECT_TRUE(gate::futures::is_dual_mode(body));
}

TEST(GateFuturesSafety, NonSinglePositionModeRefuses)
{
    auto note = gate::futures::dual_mode_refusal(accounts("false", "dual_plus"));
    ASSERT_TRUE(note.has_value());
    EXPECT_NE(note->find("position_mode"), std::string::npos);
    EXPECT_NE(note->find("dual_plus"), std::string::npos);
}

TEST(GateFuturesSafety, EmptyAccountsRefuses)
{
    auto note = gate::futures::dual_mode_refusal("");
    ASSERT_TRUE(note.has_value());
    EXPECT_NE(note->find("empty"), std::string::npos);
}

TEST(GateFuturesSafety, AccountsErrorLabelRefuses)
{
    auto note = gate::futures::dual_mode_refusal(
        R"({"label":"INVALID_KEY","message":"bad"})");
    ASSERT_TRUE(note.has_value());
    EXPECT_NE(note->find("INVALID_KEY"), std::string::npos);
}

TEST(GateFuturesSafety, MarginMismatchAdvisory)
{
    auto body = pos_body("BTC_USDT", "0.5", "isolated", "100", "50");
    auto adv = gate::futures::compute_advisories(
        body, "BTC_USDT", "crossed", /*liq_pct=*/0.0);
    ASSERT_EQ(adv.size(), 1u);
    EXPECT_EQ(adv[0].k, gate::futures::advisory::kind::margin_mode_mismatch);
    EXPECT_NE(adv[0].note.find("ISOLATED"), std::string::npos);
}

TEST(GateFuturesSafety, LiquidationCloseAdvisory)
{
    // Long, mark=100, liq=98 → distance 2% < 5% threshold.
    auto body = pos_body("BTC_USDT", "1", "cross", "100", "98");
    auto adv = gate::futures::compute_advisories(
        body, "BTC_USDT", /*expected_margin=*/"", /*liq_pct=*/0.05);
    ASSERT_EQ(adv.size(), 1u);
    EXPECT_EQ(adv[0].k, gate::futures::advisory::kind::liquidation_close);
}

TEST(GateFuturesSafety, ShortLiquidationCloseAdvisory)
{
    // Short, mark=100, liq=102 → distance 2% < 5%.
    auto body = pos_body("BTC_USDT", "-1", "cross", "100", "102");
    auto adv = gate::futures::compute_advisories(
        body, "BTC_USDT", "", 0.05);
    ASSERT_EQ(adv.size(), 1u);
    EXPECT_EQ(adv[0].k, gate::futures::advisory::kind::liquidation_close);
}

TEST(GateFuturesSafety, FlatPositionSkipped)
{
    auto body = pos_body("BTC_USDT", "0", "isolated", "100", "50");
    auto adv = gate::futures::compute_advisories(
        body, "BTC_USDT", "crossed", 0.05);
    EXPECT_TRUE(adv.empty());
}

TEST(GateFuturesSafety, ZeroLiqPriceSkipped)
{
    // Gate emits liq_price "0" when not meaningful.
    auto body = pos_body("BTC_USDT", "1", "cross", "100", "0");
    auto adv = gate::futures::compute_advisories(
        body, "BTC_USDT", "", 0.05);
    EXPECT_TRUE(adv.empty());
}

TEST(GateFuturesSafety, StrictRefusalOnlyMargin)
{
    gate::futures::advisory a;
    a.k = gate::futures::advisory::kind::liquidation_close;
    a.note = "liq";
    gate::futures::advisory b;
    b.k = gate::futures::advisory::kind::margin_mode_mismatch;
    b.note = "margin bad";
    std::vector<gate::futures::advisory> v{a, b};
    auto r = gate::futures::first_strict_refusal(v, /*strict=*/true);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "margin bad");
    EXPECT_FALSE(gate::futures::first_strict_refusal(v, false).has_value());
}

TEST(GateFuturesSafety, ArrayBodyAdvisories)
{
    std::string body = "[" + pos_body("BTC_USDT", "1", "isolated", "100", "50")
                     + "]";
    auto adv = gate::futures::compute_advisories(
        body, "BTC_USDT", "crossed", 0.0);
    ASSERT_EQ(adv.size(), 1u);
    EXPECT_EQ(adv[0].k, gate::futures::advisory::kind::margin_mode_mismatch);
}

#endif // HAS_GATE
