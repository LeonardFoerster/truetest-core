#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_futures_reconciler.h"
#include "providers/binance/binance_reconciler.h"

#include <string>

// Pure JSON-parsing helpers - the rest of the reconciler depends on a
// live REST client and is exercised via testnet integration. Keep these
// fast unit tests focused on the bits that don't.

TEST(BinanceFuturesReconciler, AvailableBalanceFromTopLevel)
{
    // Real /fapi/v2/account responses put `availableBalance` at the
    // top level (before the `assets[]` array, which can also contain a
    // field of the same name per asset). The first match must win.
    std::string body = R"({"feeTier":0,"canTrade":true,)"
                       R"("totalWalletBalance":"100.5","availableBalance":"75.25",)"
                       R"("assets":[{"asset":"USDT","availableBalance":"99.99"}]})";

    double out = 0.0;
    ASSERT_TRUE(BinanceFuturesReconciler::extract_available_balance(body, out));
    EXPECT_DOUBLE_EQ(out, 75.25);
}

TEST(BinanceFuturesReconciler, AvailableBalanceMissingReturnsFalse)
{
    std::string body = R"({"feeTier":0,"totalWalletBalance":"100.5"})";
    double out = 42.0;
    EXPECT_FALSE(BinanceFuturesReconciler::extract_available_balance(body, out));
}

TEST(BinanceFuturesReconciler, PositionAmtPositiveLong)
{
    std::string body = R"([{"symbol":"BTCUSDT","positionAmt":"0.50","entryPrice":"30000",)"
                       R"("positionSide":"BOTH"}])";
    double out = 0.0;
    ASSERT_TRUE(BinanceFuturesReconciler::extract_position_amt(body, out));
    EXPECT_DOUBLE_EQ(out, 0.50);
}

TEST(BinanceFuturesReconciler, PositionAmtNegativeShort)
{
    // Futures positionAmt is signed: long positive, short negative.
    std::string body = R"([{"symbol":"BTCUSDT","positionAmt":"-1.25",)"
                       R"("positionSide":"BOTH"}])";
    double out = 0.0;
    ASSERT_TRUE(BinanceFuturesReconciler::extract_position_amt(body, out));
    EXPECT_DOUBLE_EQ(out, -1.25);
}

TEST(BinanceFuturesReconciler, PositionAmtZeroIsParseable)
{
    std::string body = R"([{"symbol":"BTCUSDT","positionAmt":"0.0",)"
                       R"("positionSide":"BOTH"}])";
    double out = 99.0;
    ASSERT_TRUE(BinanceFuturesReconciler::extract_position_amt(body, out));
    EXPECT_DOUBLE_EQ(out, 0.0);
}

TEST(BinanceFuturesReconciler, PositionAmtMissingReturnsFalse)
{
    std::string body = R"([{"symbol":"BTCUSDT","entryPrice":"30000"}])";
    double out = 42.0;
    EXPECT_FALSE(BinanceFuturesReconciler::extract_position_amt(body, out));
}

TEST(BinanceFuturesReconciler, PositionAmtWrongSymbolRefuses)
{
    const std::string body =
        R"([{"symbol":"ETHUSDT","positionAmt":"0.0"}])";
    double out = 42.0;
    EXPECT_FALSE(BinanceFuturesReconciler::extract_position_amt(
        body, out, "BTCUSDT"));
}

TEST(BinanceFuturesReconciler, DuplicateRowDecisionMemberRefuses)
{
    double out = 42.0;
    EXPECT_FALSE(BinanceFuturesReconciler::extract_position_amt(
        R"([{"symbol":"BTCUSDT","symbol":"ETHUSDT","positionAmt":"1"}])",
        out, "BTCUSDT"));
}

TEST(BinanceFuturesReconciler, NestedOnlyDecisionMembersRefuse)
{
    double out = 42.0;
    EXPECT_FALSE(BinanceFuturesReconciler::extract_position_amt(
        R"([{"nested":{"symbol":"BTCUSDT","positionAmt":"0"}}])",
        out, "BTCUSDT"));

    double free = 0.0;
    double locked = 0.0;
    EXPECT_FALSE(BinanceReconciler::extract_balance(
        R"({"balances":[{"nested":{"asset":"BTC","free":"0","locked":"0"}}]})",
        "BTC", free, locked));
}

TEST(BinanceFuturesReconciler, MalformedSuccessPayloadRefuses)
{
    auto get = [](const std::string& endpoint, const std::string&) {
        BinanceRestClient::response r;
        r.status = 200;
        r.body = endpoint.find("positionRisk") != std::string::npos
            ? R"([{"symbol":"BTCUSDT","positionAmt":"0"}])"
            : R"(garbage "availableBalance":"0")";
        return r;
    };
    BinanceFuturesReconciler r(
        BinanceFuturesReconciler::injected_get, get, "BTCUSDT");
    portfolio p;
    EXPECT_TRUE(r.is_operational());
    EXPECT_FALSE(r.reconcile(p, 10.0).empty());
}

TEST(BinanceReconciler, MalformedAccountScannerMatchRefuses)
{
    double free = 0.0;
    double locked = 0.0;
    EXPECT_FALSE(BinanceReconciler::extract_balance(
        R"(garbage "balances":[{"asset":"USDT","free":"1000","locked":"0"}])",
        "USDT", free, locked));
}

TEST(BinanceReconciler, DuplicateBalanceDecisionMemberRefuses)
{
    double free = 0.0;
    double locked = 0.0;
    EXPECT_FALSE(BinanceReconciler::extract_balance(
        R"({"balances":[{"asset":"BTC","asset":"ETH","free":"1","locked":"0"}]})",
        "BTC", free, locked));
}

TEST(BinanceReconciler, LocalFlatVenueBaseHoldingRefuses)
{
    auto get = [](const std::string&, const std::string&) {
        BinanceRestClient::response r;
        r.status = 200;
        r.body = R"({"balances":[{"asset":"USDT","free":"1000","locked":"0"},{"asset":"BTC","free":"1","locked":"0"}]})";
        return r;
    };
    BinanceReconciler r(
        BinanceReconciler::injected_get, get,
        "BTCUSDT", "BTC", "USDT");
    portfolio p(1000.0);
    EXPECT_TRUE(r.is_operational());
    EXPECT_NE(r.reconcile(p, 10.0).find("position drift"), std::string::npos);
}

TEST(BinanceFuturesReconciler, NullRestClientReturnsError)
{
    BinanceFuturesReconciler r(nullptr, "BTCUSDT");
    portfolio p;
    EXPECT_FALSE(r.is_operational());
    auto note = r.reconcile(p, /*tolerance_bps=*/100.0);
    EXPECT_NE(note.find("no REST client"), std::string::npos);
}

TEST(BinanceFuturesReconciler, IsTestnetReflectsConstructorArg)
{
    BinanceFuturesReconciler m(nullptr, "BTCUSDT", /*is_testnet=*/false);
    BinanceFuturesReconciler t(nullptr, "BTCUSDT", /*is_testnet=*/true);
    EXPECT_FALSE(m.is_testnet());
    EXPECT_TRUE(t.is_testnet());
}

#endif // HAS_BINANCE
