#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_futures_order_encoder.h"
#include "providers/bybit/bybit_rest_client.h"
#include "providers/bybit/bybit_time_sync.h"

#include <climits>
#include <cstdlib>
#include <functional>
#include <string>

namespace {

using response = BybitRestClient::response;
using get_fn_t = std::function<response(const std::string&, const std::string&)>;

// Bybit /v5/market/time envelope (top-level time ms + result.timeSecond).
get_fn_t make_time_ok(long long server_ms, int status = 200,
                      int ret_code = 0)
{
    return [server_ms, status, ret_code](const std::string&, const std::string&) {
        response r;
        r.status = status;
        r.ret_code = ret_code;
        r.body = std::string("{\"retCode\":") + std::to_string(ret_code)
                 + ",\"retMsg\":\"OK\",\"result\":{\"timeSecond\":\""
                 + std::to_string(server_ms / 1000)
                 + "\",\"timeNano\":\"" + std::to_string(server_ms) + "000000\"},"
                 + "\"time\":" + std::to_string(server_ms) + "}";
        r.business_ok = bybit::is_business_success(status, r.body);
        return r;
    };
}

} // namespace

// --- business code detection -------------------------------------------------

TEST(BybitBusinessCode, SuccessRetCodeZero)
{
    EXPECT_TRUE(bybit::is_business_success(
        200, R"({"retCode":0,"retMsg":"OK","result":{}})"));
}

TEST(BybitBusinessCode, Http200BusinessFail)
{
    // Critical Bybit quirk: HTTP 200 + retCode != 0 is an error.
    const char* body =
        R"({"retCode":10004,"retMsg":"error sign!","result":{},"time":1})";
    EXPECT_FALSE(bybit::is_business_success(200, body));
    EXPECT_EQ(bybit::extract_ret_code(body), "10004");
}

TEST(BybitBusinessCode, Http4xxIsFail)
{
    EXPECT_FALSE(bybit::is_business_success(
        403, R"({"retCode":0,"retMsg":"OK"})"));
}

TEST(BybitBusinessCode, MissingRetCodeFailClosed)
{
    EXPECT_FALSE(bybit::is_business_success(200, R"({"retMsg":"nope"})"));
}

// --- server time parse -------------------------------------------------------

TEST(BybitServerTimeParse, TopLevelTimeMs)
{
    long long ms = 0;
    const char* body =
        R"({"retCode":0,"retMsg":"OK","result":{"timeSecond":"1683316860",)"
        R"("timeNano":"1683316860476840600"},"time":1683316860476})";
    ASSERT_TRUE(bybit::parse_server_time_ms(body, ms));
    EXPECT_EQ(ms, 1683316860476LL);
}

TEST(BybitServerTimeParse, FallbackTimeSecond)
{
    long long ms = 0;
    const char* body =
        R"({"retCode":0,"result":{"timeSecond":"1683316860"}})";
    ASSERT_TRUE(bybit::parse_server_time_ms(body, ms));
    EXPECT_EQ(ms, 1683316860000LL);
}

TEST(BybitServerTimeParse, MalformedFails)
{
    long long ms = 0;
    EXPECT_FALSE(bybit::parse_server_time_ms(R"({"retCode":0})", ms));
    EXPECT_FALSE(bybit::parse_server_time_ms(
        R"({"time":"not-a-number"})", ms));
}

// --- server_time_offset_ms via injectable get_fn -----------------------------

TEST(BybitClockSkew, OffsetFailsOnNetworkError)
{
    auto fn = [](const std::string&, const std::string&) {
        return response{0, "", -1, false};
    };
    EXPECT_EQ(BybitRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(BybitClockSkew, OffsetFailsOnHttp4xx)
{
    auto fn = make_time_ok(0, 403);
    EXPECT_EQ(BybitRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(BybitClockSkew, OffsetFailsOnMalformedBody)
{
    auto fn = [](const std::string&, const std::string&) {
        return response{200, R"({"retCode":0,"unrelated":42})", 0, true};
    };
    EXPECT_EQ(BybitRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(BybitClockSkew, OffsetNullCallableReturnsSentinel)
{
    get_fn_t empty;
    EXPECT_EQ(BybitRestClient::server_time_offset_ms(empty), LLONG_MIN);
}

TEST(BybitClockSkew, OffsetNearZeroForCurrentServerTime)
{
    const long long now = static_cast<long long>(bybit::local_time_ms());
    auto fn = make_time_ok(now);
    auto off = BybitRestClient::server_time_offset_ms(fn);
    ASSERT_NE(off, LLONG_MIN);
    EXPECT_LT(std::llabs(off), 50);
}

// --- verify_clock_skew pure logic --------------------------------------------

TEST(BybitClockSkew, VerifyOkWithinTolerance)
{
    auto r = bybit::verify_clock_skew_offset(500, 1000);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.offset_ms, 500);
}

TEST(BybitClockSkew, VerifyOkAtBoundary)
{
    auto r = bybit::verify_clock_skew_offset(-1000, 1000);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.offset_ms, -1000);
}

TEST(BybitClockSkew, VerifyFailsPositiveDrift)
{
    auto r = bybit::verify_clock_skew_offset(10'000, 1000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("drift"), std::string::npos);
}

TEST(BybitClockSkew, VerifyFailsNegativeDrift)
{
    auto r = bybit::verify_clock_skew_offset(-10'000, 1000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("drift"), std::string::npos);
}

TEST(BybitClockSkew, VerifyReportsFetchFailure)
{
    auto r = bybit::verify_clock_skew_offset(LLONG_MIN, 1000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("fetch"), std::string::npos);
}

// --- lazy-resync decision ----------------------------------------------------

TEST(BybitClockResyncDue, NeverSyncedReturnsTrue)
{
    EXPECT_TRUE(BybitRestClient::resync_due(0, 0, 300'000));
    EXPECT_TRUE(BybitRestClient::resync_due(1'000'000, 0, 300'000));
    EXPECT_TRUE(BybitRestClient::resync_due(1'000'000, -1, 300'000));
}

TEST(BybitClockResyncDue, IntervalNotElapsed)
{
    EXPECT_FALSE(BybitRestClient::resync_due(100'000, 50'000, 300'000));
}

TEST(BybitClockResyncDue, IntervalElapsed)
{
    EXPECT_TRUE(BybitRestClient::resync_due(400'000, 50'000, 300'000));
}

// --- instruments query + parse -----------------------------------------------

TEST(BybitInstrumentsQuery, LinearSymbol)
{
    EXPECT_EQ(bybit::instruments_query("linear", "BTCUSDT"),
              "category=linear&symbol=BTCUSDT");
    EXPECT_EQ(bybit::instruments_query("linear", ""),
              "category=linear");
}

TEST(BybitInstrumentsParse, TradingLinearOk)
{
    const char* body = R"({
      "retCode":0,"retMsg":"OK",
      "result":{"category":"linear","list":[{
        "symbol":"BTCUSDT","status":"Trading",
        "priceFilter":{"tickSize":"0.10"},
        "lotSizeFilter":{"qtyStep":"0.001","minOrderQty":"0.001",
                         "minNotionalValue":"5"}
      }]}
    })";
    auto p = bybit::parse_instruments_response(body, "BTCUSDT");
    EXPECT_TRUE(p.ok);
    EXPECT_TRUE(p.found);
    EXPECT_TRUE(p.trading);
    EXPECT_EQ(p.spec.symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(p.spec.tick_size, 0.10);
    EXPECT_DOUBLE_EQ(p.spec.lot_size, 0.001);
    EXPECT_DOUBLE_EQ(p.spec.min_qty, 0.001);
    EXPECT_DOUBLE_EQ(p.spec.min_notional, 5.0);
}

TEST(BybitInstrumentsParse, SymbolNotFound)
{
    const char* body = R"({
      "retCode":0,"result":{"list":[{"symbol":"ETHUSDT","status":"Trading",
        "priceFilter":{"tickSize":"0.01"},
        "lotSizeFilter":{"qtyStep":"0.01","minOrderQty":"0.01"}}]}
    })";
    auto p = bybit::parse_instruments_response(body, "BTCUSDT");
    EXPECT_FALSE(p.ok);
    EXPECT_FALSE(p.found);
    EXPECT_NE(p.note.find("not found"), std::string::npos);
}

TEST(BybitInstrumentsParse, StatusNotTrading)
{
    const char* body = R"({
      "retCode":0,"result":{"list":[{"symbol":"BTCUSDT","status":"Closed",
        "priceFilter":{"tickSize":"0.1"},
        "lotSizeFilter":{"qtyStep":"0.001","minOrderQty":"0.001"}}]}
    })";
    auto p = bybit::parse_instruments_response(body, "BTCUSDT");
    EXPECT_FALSE(p.ok);
    EXPECT_TRUE(p.found);
    EXPECT_FALSE(p.trading);
}

TEST(BybitInstrumentsParse, RetCodeFail)
{
    const char* body = R"({"retCode":10001,"retMsg":"bad"})";
    auto p = bybit::parse_instruments_response(body, "BTCUSDT");
    EXPECT_FALSE(p.ok);
    EXPECT_NE(p.note.find("retCode"), std::string::npos);
}

// --- REST prehash (path signing helper, no network) --------------------------

TEST(BybitRestPrehash, TimestampKeyWindowBody)
{
    // Docs-style: timestamp + key + recv_window + json body
    auto pre = bybit::build_rest_prehash(
        "1658384314791", "XXXXXXXXXX", "5000",
        R"({"category":"linear","symbol":"BTCUSDT"})");
    EXPECT_EQ(pre,
              R"(1658384314791XXXXXXXXXX5000{"category":"linear","symbol":"BTCUSDT"})");
}

TEST(BybitRestPrehash, EmptyPayloadEndsAtWindow)
{
    auto pre = bybit::build_rest_prehash("1", "k", "5000", "");
    EXPECT_EQ(pre, "1k5000");
}

TEST(BybitShortOrderLinkId, Within36Chars)
{
    bybit::ShortOrderLinkIdMinter m(0xabcd, 1'700'000'000'000LL);
    auto id = m.next();
    EXPECT_FALSE(id.empty());
    EXPECT_LE(id.size(), 36u);
    EXPECT_EQ(id.substr(0, 2), "tt");
    EXPECT_TRUE(BybitFuturesOrderEncoder::valid_order_link_id(id));
}

#endif // HAS_BYBIT
