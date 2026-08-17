#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_futures_safety.h"

#include <string>

namespace {

// Realistic /fapi/v2/positionRisk row, abbreviated to fields the
// advisory pass actually reads.
std::string row(const std::string& symbol,
                const std::string& positionAmt,
                const std::string& markPrice,
                const std::string& liquidationPrice,
                const std::string& marginType)
{
    std::string j = "{";
    j += R"("symbol":")"           + symbol           + R"(",)";
    j += R"("positionAmt":")"      + positionAmt      + R"(",)";
    j += R"("markPrice":")"        + markPrice        + R"(",)";
    j += R"("liquidationPrice":")" + liquidationPrice + R"(",)";
    j += R"("marginType":")"       + marginType       + R"(")";
    j += "}";
    return j;
}

std::string array_of(const std::vector<std::string>& rows)
{
    std::string out = "[";
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        if (i > 0) out += ",";
        out += rows[i];
    }
    out += "]";
    return out;
}

}

TEST(BinanceFuturesSafety, EmptyArrayProducesNoAdvisories)
{
    auto a = binance::futures::compute_advisories("[]", "ISOLATED", 0.05);
    EXPECT_TRUE(a.empty());
}

TEST(BinanceFuturesSafety, ZeroPositionAmtSkipsRow)
{
    // Even with a "wrong" margin type and a close liquidation price,
    // a flat position is irrelevant - skip silently.
    auto body = array_of({
        row("BTCUSDT", "0.0", "30000", "29900", "cross"),
    });
    auto a = binance::futures::compute_advisories(body, "ISOLATED", 0.05);
    EXPECT_TRUE(a.empty());
}

TEST(BinanceFuturesSafety, MarginModeMismatchFlagged)
{
    auto body = array_of({
        row("BTCUSDT", "0.5", "30000", "27000", "cross"),
    });
    auto a = binance::futures::compute_advisories(body, "ISOLATED", 0.0);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].k, binance::futures::advisory::kind::margin_mode_mismatch);
    EXPECT_EQ(a[0].symbol, "BTCUSDT");
    EXPECT_NE(a[0].note.find("CROSSED"), std::string::npos);
    EXPECT_NE(a[0].note.find("ISOLATED"), std::string::npos);
}

TEST(BinanceFuturesSafety, MarginModeMatchSilent)
{
    // Both spellings of "isolated" should canonicalize equally.
    for (const auto* venue : {"isolated", "ISOLATED"})
    {
        auto body = array_of({
            row("BTCUSDT", "0.5", "30000", "27000", venue),
        });
        auto a = binance::futures::compute_advisories(body, "ISOLATED", 0.0);
        EXPECT_EQ(a.size(), 0u) << "venue=" << venue;
    }
}

TEST(BinanceFuturesSafety, EmptyExpectedMarginTypeDisablesCheck)
{
    // "cross" venue but operator didn't configure an expectation: silent.
    auto body = array_of({
        row("BTCUSDT", "0.5", "30000", "27000", "cross"),
    });
    auto a = binance::futures::compute_advisories(body, "", 0.0);
    EXPECT_TRUE(a.empty());
}

TEST(BinanceFuturesSafety, LongCloseToLiquidationFlagged)
{
    // Long 0.5 BTC, mark 30000, liq 28800 -> distance = 4% < 5% threshold.
    auto body = array_of({
        row("BTCUSDT", "0.5", "30000", "28800", "isolated"),
    });
    auto a = binance::futures::compute_advisories(body, "", 0.05);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].k, binance::futures::advisory::kind::liquidation_close);
    EXPECT_EQ(a[0].symbol, "BTCUSDT");
}

TEST(BinanceFuturesSafety, LongFarFromLiquidationSilent)
{
    // Long 0.5 BTC, mark 30000, liq 25000 -> distance ~16.7% >> 5%.
    auto body = array_of({
        row("BTCUSDT", "0.5", "30000", "25000", "isolated"),
    });
    auto a = binance::futures::compute_advisories(body, "", 0.05);
    EXPECT_TRUE(a.empty());
}

TEST(BinanceFuturesSafety, ShortCloseToLiquidationFlagged)
{
    // Short -0.5 BTC, mark 30000, liq 31200 -> distance = 4% < 5%.
    auto body = array_of({
        row("BTCUSDT", "-0.5", "30000", "31200", "isolated"),
    });
    auto a = binance::futures::compute_advisories(body, "", 0.05);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].k, binance::futures::advisory::kind::liquidation_close);
}

TEST(BinanceFuturesSafety, ZeroLiquidationPriceTolerated)
{
    // Unfunded testnet account: liquidationPrice=0 means "not computed".
    // Silent - flagging would train operators to ignore advisories.
    auto body = array_of({
        row("BTCUSDT", "0.5", "30000", "0", "isolated"),
    });
    auto a = binance::futures::compute_advisories(body, "", 0.05);
    EXPECT_TRUE(a.empty());
}

TEST(BinanceFuturesSafety, ZeroMarkPriceTolerated)
{
    // Same idea: missing/zero mark price -> skip the liquidation check.
    auto body = array_of({
        row("BTCUSDT", "0.5", "0", "27000", "isolated"),
    });
    auto a = binance::futures::compute_advisories(body, "", 0.05);
    EXPECT_TRUE(a.empty());
}

TEST(BinanceFuturesSafety, LiquidationPctZeroDisablesCheck)
{
    // Mark right next to liquidation, but threshold disabled -> silent.
    auto body = array_of({
        row("BTCUSDT", "0.5", "30000", "29999", "isolated"),
    });
    auto a = binance::futures::compute_advisories(body, "", 0.0);
    EXPECT_TRUE(a.empty());
}

TEST(BinanceFuturesSafety, FirstStrictRefusalOffByDefault)
{
    std::vector<binance::futures::advisory> advisories;
    binance::futures::advisory a;
    a.k = binance::futures::advisory::kind::margin_mode_mismatch;
    a.note = "BTCUSDT margin mode is CROSSED, operator configured ISOLATED";
    advisories.push_back(a);

    auto note = binance::futures::first_strict_refusal(advisories,
                                                       /*strict=*/false);
    EXPECT_FALSE(note.has_value());
}

TEST(BinanceFuturesSafety, FirstStrictRefusalOnMarginMismatch)
{
    std::vector<binance::futures::advisory> advisories;
    binance::futures::advisory a;
    a.k = binance::futures::advisory::kind::margin_mode_mismatch;
    a.note = "BTCUSDT margin mode is CROSSED, operator configured ISOLATED";
    advisories.push_back(a);

    auto note = binance::futures::first_strict_refusal(advisories,
                                                       /*strict=*/true);
    ASSERT_TRUE(note.has_value());
    EXPECT_NE(note->find("CROSSED"), std::string::npos);
}

TEST(BinanceFuturesSafety, FirstStrictRefusalIgnoresLiquidationAdvisories)
{
    // Strict mode escalates margin-mode only. Liquidation advisories
    // remain warnings - the operator can't unilaterally reduce
    // distance-to-liquidation by typing a flag, so refusing on it
    // would trap them in a useless gate.
    std::vector<binance::futures::advisory> advisories;
    binance::futures::advisory a;
    a.k = binance::futures::advisory::kind::liquidation_close;
    a.note = "BTCUSDT position is 2.00% from liquidation";
    advisories.push_back(a);

    auto note = binance::futures::first_strict_refusal(advisories,
                                                       /*strict=*/true);
    EXPECT_FALSE(note.has_value());
}

TEST(BinanceFuturesSafety, FirstStrictRefusalReturnsFirstMarginMismatch)
{
    std::vector<binance::futures::advisory> advisories;
    binance::futures::advisory a1, a2;
    a1.k = binance::futures::advisory::kind::liquidation_close;
    a1.note = "ETHUSDT liq advisory";
    a2.k = binance::futures::advisory::kind::margin_mode_mismatch;
    a2.note = "BTCUSDT margin mismatch";
    advisories.push_back(a1);
    advisories.push_back(a2);

    auto note = binance::futures::first_strict_refusal(advisories,
                                                       /*strict=*/true);
    ASSERT_TRUE(note.has_value());
    EXPECT_NE(note->find("BTCUSDT"), std::string::npos);
}

TEST(BinanceFuturesSafety, BothChecksCombineAcrossRows)
{
    auto body = array_of({
        // Margin-mode mismatch on BTCUSDT (cross vs ISOLATED expected).
        row("BTCUSDT", "0.5", "30000", "27000", "cross"),
        // Liquidation-distance hit on ETHUSDT (long, 2% from liq).
        row("ETHUSDT", "1.0", "2000",  "1960",  "isolated"),
    });
    auto a = binance::futures::compute_advisories(body, "ISOLATED", 0.05);
    ASSERT_EQ(a.size(), 2u);

    // Order isn't strictly guaranteed by the helper; index by symbol.
    bool saw_margin = false, saw_liq = false;
    for (const auto& adv : a)
    {
        if (adv.k == binance::futures::advisory::kind::margin_mode_mismatch &&
            adv.symbol == "BTCUSDT")
            saw_margin = true;
        if (adv.k == binance::futures::advisory::kind::liquidation_close &&
            adv.symbol == "ETHUSDT")
            saw_liq = true;
    }
    EXPECT_TRUE(saw_margin);
    EXPECT_TRUE(saw_liq);
}

TEST(BinanceFuturesSafety, StrictMarginProbeRequiresAuthoritativeEvidence)
{
    using binance::futures::strict_margin_probe_refusal;
    EXPECT_TRUE(strict_margin_probe_refusal(
        503, R"([])", "BTCUSDT", "ISOLATED", true).has_value());

    for (const std::string_view body : {
             std::string_view{"not-json"},
             std::string_view{R"({"symbol":"BTCUSDT"})"},
             std::string_view{R"([{"symbol":"BTCUSDT","positionAmt":"0"}])"},
             std::string_view{R"([{"symbol":"BTCUSDT","positionAmt":"0","marginType":"isolated","marginType":"cross"}])"},
             std::string_view{R"([{"nested":{"symbol":"BTCUSDT","positionAmt":"0","marginType":"isolated"}}])"},
             std::string_view{R"([{"symbol":"ETHUSDT","positionAmt":"0","marginType":"isolated"}])"}})
    {
        EXPECT_TRUE(strict_margin_probe_refusal(
            200, body, "BTCUSDT", "ISOLATED", true).has_value()) << body;
    }
}

TEST(BinanceFuturesSafety, StrictMarginProbeAcceptsOnlyMatchingScopedRow)
{
    using binance::futures::strict_margin_probe_refusal;
    EXPECT_TRUE(strict_margin_probe_refusal(
        200, R"([])", "BTCUSDT", "ISOLATED", true).has_value());
    EXPECT_FALSE(strict_margin_probe_refusal(
        200,
        R"([{"symbol":"BTCUSDT","positionAmt":"0","marginType":"isolated"}])",
        "BTCUSDT", "ISOLATED", true).has_value());
    EXPECT_TRUE(strict_margin_probe_refusal(
        200,
        R"([{"symbol":"BTCUSDT","positionAmt":"1","marginType":"cross"}])",
        "BTCUSDT", "ISOLATED", true).has_value());
    EXPECT_TRUE(strict_margin_probe_refusal(
        200,
        R"([{"symbol":"BTCUSDT","positionAmt":"0","marginType":"incorrect"}])",
        "BTCUSDT", "ISOLATED", true).has_value());
    EXPECT_TRUE(strict_margin_probe_refusal(
        200,
        R"([{"symbol":"BTCUSDT","positionAmt":"0","marginType":"isolated"}])",
        "BTCUSDT", "INVALID", true).has_value());
    EXPECT_FALSE(strict_margin_probe_refusal(
        503, "broken", "BTCUSDT", "ISOLATED", false).has_value());
}

#endif // HAS_BINANCE
