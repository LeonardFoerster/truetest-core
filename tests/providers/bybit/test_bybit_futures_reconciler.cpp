#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_futures_reconciler.h"

#include <string>
#include <unordered_map>

namespace {

BybitRestClient::response ok_body(std::string body)
{
    BybitRestClient::response r;
    r.status = 200;
    r.body = std::move(body);
    r.ret_code = 0;
    r.business_ok = true;
    return r;
}

BybitRestClient::response http_fail(int status, std::string body = "err")
{
    BybitRestClient::response r;
    r.status = status;
    r.body = std::move(body);
    r.business_ok = false;
    return r;
}

BybitRestClient::response business_fail(std::string body)
{
    BybitRestClient::response r;
    r.status = 200;
    r.body = std::move(body);
    r.ret_code = 10001;
    r.business_ok = false;
    return r;
}

// UNIFIED wallet with totalAvailableBalance + coin[].
std::string wallet_json(const char* available)
{
    return std::string(R"({"retCode":0,"retMsg":"OK","result":{"list":[{)")
         + R"("totalAvailableBalance":")" + available
         + R"(","coin":[{"coin":"USDT","walletBalance":")" + available
         + R"(","availableToWithdraw":")" + available + R"("}]}]}})";
}

// Position list with one linear row (side Buy/Sell + size).
std::string pos_json(const char* size, const char* side = "Buy",
                     const char* symbol = "BTCUSDT")
{
    return std::string(R"({"retCode":0,"retMsg":"OK","result":{"list":[{)")
         + R"("symbol":")" + symbol + R"(","side":")" + side
         + R"(","size":")" + size + R"(","positionIdx":0}]}})";
}

std::string pos_empty()
{
    return R"({"retCode":0,"retMsg":"OK","result":{"list":[]}})";
}

} // namespace

// --- extract_available_usdt ---

TEST(BybitFuturesReconciler, AvailableUsdtFromTotalAvailableBalance)
{
    const std::string body = wallet_json("75.25");
    double out = 0.0;
    ASSERT_TRUE(BybitFuturesReconciler::extract_available_usdt(body, out));
    EXPECT_DOUBLE_EQ(out, 75.25);
}

TEST(BybitFuturesReconciler, AvailableUsdtFromCoinArrayFallback)
{
    std::string body =
        R"({"retCode":0,"result":{"list":[{"coin":[)"
        R"({"coin":"USDT","availableToWithdraw":"10.5","walletBalance":"99.9"}]"
        R"(}]}})";
    double out = 0.0;
    ASSERT_TRUE(BybitFuturesReconciler::extract_available_usdt(body, out));
    EXPECT_DOUBLE_EQ(out, 10.5);
}

TEST(BybitFuturesReconciler, AvailableUsdtMissingReturnsFalse)
{
    std::string body =
        R"({"retCode":0,"result":{"list":[{"coin":[{"coin":"BTC","walletBalance":"1"}]}]}})";
    double out = 42.0;
    EXPECT_FALSE(BybitFuturesReconciler::extract_available_usdt(body, out));
}

// --- extract_position_amt ---

TEST(BybitFuturesReconciler, PositionAmtPositiveBuy)
{
    double out = 0.0;
    ASSERT_TRUE(BybitFuturesReconciler::extract_position_amt(
        pos_json("0.50", "Buy"), out, "BTCUSDT"));
    EXPECT_DOUBLE_EQ(out, 0.50);
}

TEST(BybitFuturesReconciler, PositionAmtNegativeSell)
{
    double out = 0.0;
    ASSERT_TRUE(BybitFuturesReconciler::extract_position_amt(
        pos_json("1.25", "Sell"), out, "BTCUSDT"));
    EXPECT_DOUBLE_EQ(out, -1.25);
}

TEST(BybitFuturesReconciler, PositionAmtEmptyListIsFlatZero)
{
    double out = 99.0;
    ASSERT_TRUE(BybitFuturesReconciler::extract_position_amt(
        pos_empty(), out, "BTCUSDT"));
    EXPECT_DOUBLE_EQ(out, 0.0);
}

TEST(BybitFuturesReconciler, PositionAmtOtherSymbolsOnlyRefuses)
{
    const std::string body = pos_json("1.5", "Buy", "ETHUSDT");
    double out = 99.0;
    EXPECT_FALSE(BybitFuturesReconciler::extract_position_amt(
        body, out, "BTCUSDT"));
}

TEST(BybitFuturesReconciler, PositionAmtZeroIsParseable)
{
    double out = 99.0;
    ASSERT_TRUE(BybitFuturesReconciler::extract_position_amt(
        pos_json("0.0", "Buy"), out, "BTCUSDT"));
    EXPECT_DOUBLE_EQ(out, 0.0);
}

// --- reconcile via injected get_fn ---

TEST(BybitFuturesReconciler, NullGetReturnsError)
{
    BybitFuturesReconciler r(BybitFuturesReconciler::get_fn{}, "BTCUSDT");
    portfolio p(100.0);
    auto note = r.reconcile(p, /*tolerance_bps=*/100.0);
    EXPECT_NE(note.find("no REST client"), std::string::npos);
}

TEST(BybitFuturesReconciler, MatchPasses)
{
    auto get = [](const std::string& ep, const std::string& /*q*/) {
        if (ep.find("position/list") != std::string::npos)
            return ok_body(pos_json("0.1", "Buy"));
        if (ep.find("wallet-balance") != std::string::npos)
            return ok_body(wallet_json("1000"));
        return http_fail(404);
    };

    BybitFuturesReconciler r(get, "BTCUSDT");
    portfolio p;
    std::unordered_map<std::string, position> pos;
    pos["BTCUSDT"] = position{0.1, 0.0};
    p.restore_state(/*cash=*/1000.0, /*trades=*/0, std::move(pos));

    EXPECT_TRUE(r.reconcile(p, /*tolerance_bps=*/10.0).empty());
}

TEST(BybitFuturesReconciler, CashMismatchFails)
{
    auto get = [](const std::string& ep, const std::string& /*q*/) {
        if (ep.find("position/list") != std::string::npos)
            return ok_body(pos_empty());
        if (ep.find("wallet-balance") != std::string::npos)
            return ok_body(wallet_json("50"));
        return http_fail(404);
    };

    BybitFuturesReconciler r(get, "BTCUSDT");
    portfolio p(1000.0);
    auto note = r.reconcile(p, /*tolerance_bps=*/10.0);
    EXPECT_NE(note.find("cash drift"), std::string::npos);
}

TEST(BybitFuturesReconciler, PositionMismatchFails)
{
    auto get = [](const std::string& ep, const std::string& /*q*/) {
        if (ep.find("position/list") != std::string::npos)
            return ok_body(pos_json("2.0", "Buy"));
        if (ep.find("wallet-balance") != std::string::npos)
            return ok_body(wallet_json("1000"));
        return http_fail(404);
    };

    BybitFuturesReconciler r(get, "BTCUSDT");
    portfolio p;
    std::unordered_map<std::string, position> pos;
    pos["BTCUSDT"] = position{0.1, 0.0};
    p.restore_state(/*cash=*/1000.0, /*trades=*/0, std::move(pos));

    auto note = r.reconcile(p, /*tolerance_bps=*/10.0);
    EXPECT_NE(note.find("position drift"), std::string::npos);
}

TEST(BybitFuturesReconciler, HttpFailureFailsClosed)
{
    auto get = [](const std::string& ep, const std::string& /*q*/) {
        if (ep.find("position/list") != std::string::npos)
            return http_fail(503, "unavailable");
        return ok_body(wallet_json("1000"));
    };

    BybitFuturesReconciler r(get, "BTCUSDT");
    portfolio p(1000.0);
    auto note = r.reconcile(p, 100.0);
    EXPECT_NE(note.find("HTTP 503"), std::string::npos);
}

TEST(BybitFuturesReconciler, BusinessCodeFailureFailsClosed)
{
    auto get = [](const std::string& ep, const std::string& /*q*/) {
        if (ep.find("position/list") != std::string::npos)
            return ok_body(pos_empty());
        if (ep.find("wallet-balance") != std::string::npos)
            return business_fail(
                R"({"retCode":10001,"retMsg":"invalid","result":null})");
        return http_fail(404);
    };

    BybitFuturesReconciler r(get, "BTCUSDT");
    portfolio p(1000.0);
    auto note = r.reconcile(p, 100.0);
    EXPECT_NE(note.find("retCode"), std::string::npos);
    EXPECT_NE(note.find("10001"), std::string::npos);
}

TEST(BybitFuturesReconciler, IsDemoReflectsConstructorArg)
{
    BybitFuturesReconciler m(BybitFuturesReconciler::get_fn{}, "BTCUSDT",
                             "linear", /*is_demo=*/false);
    BybitFuturesReconciler d(BybitFuturesReconciler::get_fn{}, "BTCUSDT",
                             "linear", /*is_demo=*/true);
    EXPECT_FALSE(m.is_demo());
    EXPECT_TRUE(d.is_demo());
}

#endif // HAS_BYBIT
