#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_futures_reconciler.h"

#include <string>
#include <unordered_map>

namespace {

BitgetRestClient::response ok_body(std::string body)
{
    BitgetRestClient::response r;
    r.status = 200;
    r.body = std::move(body);
    r.business_ok = true;
    return r;
}

BitgetRestClient::response http_fail(int status, std::string body = "err")
{
    BitgetRestClient::response r;
    r.status = status;
    r.body = std::move(body);
    r.business_ok = false;
    return r;
}

BitgetRestClient::response business_fail(std::string body)
{
    BitgetRestClient::response r;
    r.status = 200;
    r.body = std::move(body);
    r.business_ok = false;
    return r;
}

// Flat assets envelope with USDT available.
std::string assets_json(const char* available)
{
    return std::string(R"({"code":"00000","msg":"success","data":{"assets":[)")
         + R"({"coin":"USDT","available":")" + available + R"(","equity":")"
         + available + R"("},{"coin":"BTC","available":"0.1"}]}})";
}

// Position list with one row.
std::string pos_json(const char* total, const char* pos_side = "long",
                     const char* symbol = "BTCUSDT")
{
    return std::string(R"({"code":"00000","msg":"success","data":{"list":[)")
         + R"({"symbol":")" + symbol + R"(","total":")" + total
         + R"(","size":")" + total + R"(","posSide":")" + pos_side
         + R"(","marginMode":"crossed"}]}})";
}

std::string pos_empty()
{
    return R"({"code":"00000","msg":"success","data":{"list":[]}})";
}

} // namespace

// --- extract_available_usdt ---

TEST(BitgetFuturesReconciler, AvailableUsdtFromAssetsArray)
{
    const std::string body = assets_json("75.25");
    double out = 0.0;
    ASSERT_TRUE(BitgetFuturesReconciler::extract_available_usdt(body, out));
    EXPECT_DOUBLE_EQ(out, 75.25);
}

TEST(BitgetFuturesReconciler, AvailableUsdtPrefersAvailableOverEquity)
{
    std::string body =
        R"({"code":"00000","data":{"assets":[{"coin":"USDT",)"
        R"("available":"10.5","availableEquity":"99.9"}]}})";
    double out = 0.0;
    ASSERT_TRUE(BitgetFuturesReconciler::extract_available_usdt(body, out));
    EXPECT_DOUBLE_EQ(out, 10.5);
}

TEST(BitgetFuturesReconciler, AvailableUsdtMissingReturnsFalse)
{
    std::string body =
        R"({"code":"00000","data":{"assets":[{"coin":"BTC","available":"1"}]}})";
    double out = 42.0;
    EXPECT_FALSE(BitgetFuturesReconciler::extract_available_usdt(body, out));
}

// --- extract_position_amt ---

TEST(BitgetFuturesReconciler, PositionAmtPositiveLong)
{
    double out = 0.0;
    ASSERT_TRUE(BitgetFuturesReconciler::extract_position_amt(
        pos_json("0.50", "long"), out, "BTCUSDT"));
    EXPECT_DOUBLE_EQ(out, 0.50);
}

TEST(BitgetFuturesReconciler, PositionAmtNegativeShort)
{
    double out = 0.0;
    ASSERT_TRUE(BitgetFuturesReconciler::extract_position_amt(
        pos_json("1.25", "short"), out, "BTCUSDT"));
    EXPECT_DOUBLE_EQ(out, -1.25);
}

TEST(BitgetFuturesReconciler, PositionAmtEmptyListIsFlatZero)
{
    double out = 99.0;
    ASSERT_TRUE(BitgetFuturesReconciler::extract_position_amt(
        pos_empty(), out, "BTCUSDT"));
    EXPECT_DOUBLE_EQ(out, 0.0);
}

TEST(BitgetFuturesReconciler, PositionAmtZeroIsParseable)
{
    double out = 99.0;
    ASSERT_TRUE(BitgetFuturesReconciler::extract_position_amt(
        pos_json("0.0", "long"), out, "BTCUSDT"));
    EXPECT_DOUBLE_EQ(out, 0.0);
}

TEST(BitgetFuturesReconciler, PositionAmtSignedSizeWithoutSide)
{
    // One-way may already emit signed size with empty/net posSide.
    std::string body =
        R"({"code":"00000","data":{"list":[{"symbol":"BTCUSDT",)"
        R"("total":"-0.3","posSide":""}]}})";
    double out = 0.0;
    ASSERT_TRUE(BitgetFuturesReconciler::extract_position_amt(
        body, out, "BTCUSDT"));
    EXPECT_DOUBLE_EQ(out, -0.3);
}

// --- reconcile via injected get_fn ---

TEST(BitgetFuturesReconciler, NullGetReturnsError)
{
    BitgetFuturesReconciler r(BitgetFuturesReconciler::get_fn{}, "BTCUSDT");
    portfolio p(100.0);
    auto note = r.reconcile(p, /*tolerance_bps=*/100.0);
    EXPECT_NE(note.find("no REST client"), std::string::npos);
}

TEST(BitgetFuturesReconciler, MatchPasses)
{
    auto get = [](const std::string& ep, const std::string& /*q*/) {
        if (ep.find("current-position") != std::string::npos)
            return ok_body(pos_json("0.1", "long"));
        if (ep.find("assets") != std::string::npos)
            return ok_body(assets_json("1000"));
        return http_fail(404);
    };

    BitgetFuturesReconciler r(get, "BTCUSDT");
    portfolio p;
    std::unordered_map<std::string, position> pos;
    pos["BTCUSDT"] = position{0.1, 0.0};
    p.restore_state(/*cash=*/1000.0, /*trades=*/0, std::move(pos));

    EXPECT_TRUE(r.reconcile(p, /*tolerance_bps=*/10.0).empty());
}

TEST(BitgetFuturesReconciler, CashMismatchFails)
{
    auto get = [](const std::string& ep, const std::string& /*q*/) {
        if (ep.find("current-position") != std::string::npos)
            return ok_body(pos_empty());
        if (ep.find("assets") != std::string::npos)
            return ok_body(assets_json("50"));
        return http_fail(404);
    };

    BitgetFuturesReconciler r(get, "BTCUSDT");
    portfolio p(1000.0); // local cash 1000 vs venue 50
    auto note = r.reconcile(p, /*tolerance_bps=*/10.0);
    EXPECT_NE(note.find("cash drift"), std::string::npos);
}

TEST(BitgetFuturesReconciler, PositionMismatchFails)
{
    auto get = [](const std::string& ep, const std::string& /*q*/) {
        if (ep.find("current-position") != std::string::npos)
            return ok_body(pos_json("2.0", "long"));
        if (ep.find("assets") != std::string::npos)
            return ok_body(assets_json("1000"));
        return http_fail(404);
    };

    BitgetFuturesReconciler r(get, "BTCUSDT");
    portfolio p;
    std::unordered_map<std::string, position> pos;
    pos["BTCUSDT"] = position{0.1, 0.0}; // local 0.1 vs venue 2.0
    p.restore_state(/*cash=*/1000.0, /*trades=*/0, std::move(pos));

    auto note = r.reconcile(p, /*tolerance_bps=*/10.0);
    EXPECT_NE(note.find("position drift"), std::string::npos);
}

TEST(BitgetFuturesReconciler, HttpFailureFailsClosed)
{
    auto get = [](const std::string& ep, const std::string& /*q*/) {
        if (ep.find("current-position") != std::string::npos)
            return http_fail(503, "unavailable");
        return ok_body(assets_json("1000"));
    };

    BitgetFuturesReconciler r(get, "BTCUSDT");
    portfolio p(1000.0);
    auto note = r.reconcile(p, 100.0);
    EXPECT_NE(note.find("HTTP 503"), std::string::npos);
}

TEST(BitgetFuturesReconciler, BusinessCodeFailureFailsClosed)
{
    auto get = [](const std::string& ep, const std::string& /*q*/) {
        if (ep.find("current-position") != std::string::npos)
            return ok_body(pos_empty());
        if (ep.find("assets") != std::string::npos)
            return business_fail(
                R"({"code":"40014","msg":"invalid","data":null})");
        return http_fail(404);
    };

    BitgetFuturesReconciler r(get, "BTCUSDT");
    portfolio p(1000.0);
    auto note = r.reconcile(p, 100.0);
    EXPECT_NE(note.find("business code"), std::string::npos);
    EXPECT_NE(note.find("40014"), std::string::npos);
}

TEST(BitgetFuturesReconciler, IsDemoReflectsConstructorArg)
{
    BitgetFuturesReconciler m(BitgetFuturesReconciler::get_fn{}, "BTCUSDT",
                              "USDT-FUTURES", /*is_demo=*/false);
    BitgetFuturesReconciler d(BitgetFuturesReconciler::get_fn{}, "BTCUSDT",
                              "USDT-FUTURES", /*is_demo=*/true);
    EXPECT_FALSE(m.is_demo());
    EXPECT_TRUE(d.is_demo());
}

#endif // HAS_BITGET
