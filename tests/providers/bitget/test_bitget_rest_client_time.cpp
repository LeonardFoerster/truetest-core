#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_rest_client.h"
#include "providers/bitget/bitget_time_sync.h"

#include <climits>
#include <cstdlib>
#include <functional>
#include <string>

namespace {

using response = BitgetRestClient::response;
using get_fn_t = std::function<response(const std::string&, const std::string&)>;

// Bitget /api/v2/public/time envelope (serverTime as string, as live).
get_fn_t make_time_ok(long long server_ms, int status = 200,
                      const char* code = "00000")
{
    return [server_ms, status, code](const std::string&, const std::string&) {
        response r;
        r.status = status;
        r.body = std::string("{\"code\":\"") + code
                 + "\",\"msg\":\"success\",\"requestTime\":"
                 + std::to_string(server_ms)
                 + ",\"data\":{\"serverTime\":\""
                 + std::to_string(server_ms) + "\"}}";
        r.business_ok = bitget::is_business_success(status, r.body);
        return r;
    };
}

} // namespace

// --- query sort (prehash) ----------------------------------------------------

TEST(BitgetQuerySort, SortsKeysAlphabetically)
{
    EXPECT_EQ(bitget::sort_query_string("symbol=BTCUSDT&category=USDT-FUTURES"),
              "category=USDT-FUTURES&symbol=BTCUSDT");
    EXPECT_EQ(bitget::sort_query_string("category=USDT-FUTURES&symbol=BTCUSDT"),
              "category=USDT-FUTURES&symbol=BTCUSDT");
    EXPECT_EQ(bitget::sort_query_string("z=1&a=2&m=3"), "a=2&m=3&z=1");
    EXPECT_EQ(bitget::sort_query_string(""), "");
    EXPECT_EQ(bitget::sort_query_string("only=one"), "only=one");
}

// --- business code detection -------------------------------------------------

TEST(BitgetBusinessCode, SuccessCode00000)
{
    EXPECT_TRUE(bitget::is_business_success(
        200, R"({"code":"00000","msg":"success","data":{}})"));
}

TEST(BitgetBusinessCode, Http200BusinessFail)
{
    // Critical Bitget quirk: HTTP 200 + code != "00000" is an error.
    const char* body =
        R"({"code":"40009","msg":"sign signature error","requestTime":1,"data":null})";
    EXPECT_FALSE(bitget::is_business_success(200, body));
    EXPECT_EQ(bitget::extract_business_code(body), "40009");
}

TEST(BitgetBusinessCode, Http4xxIsFail)
{
    EXPECT_FALSE(bitget::is_business_success(
        403, R"({"code":"00000","msg":"success"})"));
}

TEST(BitgetBusinessCode, MissingCodeFailClosed)
{
    EXPECT_FALSE(bitget::is_business_success(200, R"({"msg":"nope"})"));
}

// --- server time parse -------------------------------------------------------

TEST(BitgetServerTimeParse, StringServerTimeInData)
{
    long long ms = 0;
    const char* body =
        R"({"code":"00000","msg":"success","requestTime":1688008631614,)"
        R"("data":{"serverTime":"1688008631614"}})";
    ASSERT_TRUE(bitget::parse_server_time_ms(body, ms));
    EXPECT_EQ(ms, 1688008631614LL);
}

TEST(BitgetServerTimeParse, NumericServerTime)
{
    long long ms = 0;
    const char* body =
        R"({"code":"00000","data":{"serverTime":1710000000000}})";
    ASSERT_TRUE(bitget::parse_server_time_ms(body, ms));
    EXPECT_EQ(ms, 1710000000000LL);
}

TEST(BitgetServerTimeParse, FallbackRequestTime)
{
    long long ms = 0;
    // No serverTime key — fall back to requestTime.
    const char* body = R"({"code":"00000","requestTime":99,"data":{}})";
    ASSERT_TRUE(bitget::parse_server_time_ms(body, ms));
    EXPECT_EQ(ms, 99LL);
}

TEST(BitgetServerTimeParse, MalformedFails)
{
    long long ms = 0;
    EXPECT_FALSE(bitget::parse_server_time_ms(R"({"code":"00000"})", ms));
    EXPECT_FALSE(bitget::parse_server_time_ms(
        R"({"serverTime":"not-a-number"})", ms));
}

// --- server_time_offset_ms via injectable get_fn -----------------------------

TEST(BitgetClockSkew, OffsetFailsOnNetworkError)
{
    auto fn = [](const std::string&, const std::string&) {
        return response{0, "", false};
    };
    EXPECT_EQ(BitgetRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(BitgetClockSkew, OffsetFailsOnHttp4xx)
{
    auto fn = make_time_ok(0, 403);
    EXPECT_EQ(BitgetRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(BitgetClockSkew, OffsetFailsOnMalformedBody)
{
    auto fn = [](const std::string&, const std::string&) {
        return response{200, R"({"code":"00000","unrelated":42})", true};
    };
    EXPECT_EQ(BitgetRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(BitgetClockSkew, OffsetNullCallableReturnsSentinel)
{
    get_fn_t empty;
    EXPECT_EQ(BitgetRestClient::server_time_offset_ms(empty), LLONG_MIN);
}

TEST(BitgetClockSkew, OffsetNearZeroForCurrentServerTime)
{
    // Inject server time ≈ local so offset is small (not LLONG_MIN).
    const long long now = static_cast<long long>(bitget::local_time_ms());
    auto fn = make_time_ok(now);
    auto off = BitgetRestClient::server_time_offset_ms(fn);
    ASSERT_NE(off, LLONG_MIN);
    EXPECT_LT(std::llabs(off), 50); // allow a few ms of test scheduling
}

// --- verify_clock_skew pure logic --------------------------------------------

TEST(BitgetClockSkew, VerifyOkWithinTolerance)
{
    auto r = bitget::verify_clock_skew_offset(500, 2000);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.offset_ms, 500);
}

TEST(BitgetClockSkew, VerifyOkAtBoundary)
{
    auto r = bitget::verify_clock_skew_offset(-2000, 2000);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.offset_ms, -2000);
}

TEST(BitgetClockSkew, VerifyFailsPositiveDrift)
{
    auto r = bitget::verify_clock_skew_offset(10'000, 2000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("drift"), std::string::npos);
}

TEST(BitgetClockSkew, VerifyFailsNegativeDrift)
{
    auto r = bitget::verify_clock_skew_offset(-10'000, 2000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("drift"), std::string::npos);
}

TEST(BitgetClockSkew, VerifyReportsFetchFailure)
{
    auto r = bitget::verify_clock_skew_offset(LLONG_MIN, 2000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("fetch"), std::string::npos);
}

// --- lazy-resync decision ----------------------------------------------------

TEST(BitgetClockResyncDue, NeverSyncedReturnsTrue)
{
    EXPECT_TRUE(BitgetRestClient::resync_due(0, 0, 300'000));
    EXPECT_TRUE(BitgetRestClient::resync_due(1'000'000, 0, 300'000));
    EXPECT_TRUE(BitgetRestClient::resync_due(1'000'000, -1, 300'000));
}

TEST(BitgetClockResyncDue, WithinIntervalReturnsFalse)
{
    EXPECT_FALSE(BitgetRestClient::resync_due(360'000, 300'000, 300'000));
}

TEST(BitgetClockResyncDue, AtIntervalBoundaryIsDue)
{
    EXPECT_TRUE(BitgetRestClient::resync_due(600'000, 300'000, 300'000));
}

TEST(BitgetClockResyncDue, NonPositiveIntervalDisablesLazySync)
{
    EXPECT_FALSE(BitgetRestClient::resync_due(10'000'000, 1, 0));
    EXPECT_FALSE(BitgetRestClient::resync_due(10'000'000, 1, -5));
}

// --- instruments probe (canned JSON) ----------------------------------------

namespace {

// Minimal UTA instruments envelope matching live field names.
constexpr const char* kInstrumentsBtcOk =
    R"({"code":"00000","msg":"success","requestTime":1,"data":[)"
    R"({"symbol":"BTCUSDT","category":"USDT-FUTURES","status":"online",)"
    R"("priceMultiplier":"0.1","quantityMultiplier":"0.001",)"
    R"("minOrderQty":"0.001","minOrderAmount":"5",)"
    R"("makerFeeRate":"0.0002","takerFeeRate":"0.0006"})"
    R"(]})";

constexpr const char* kInstrumentsOffline =
    R"({"code":"00000","msg":"success","data":[)"
    R"({"symbol":"BTCUSDT","status":"offline",)"
    R"("priceMultiplier":"0.1","quantityMultiplier":"0.001"})"
    R"(]})";

constexpr const char* kInstrumentsBusinessFail =
    R"({"code":"40001","msg":"invalid","data":null})";

} // namespace

TEST(BitgetInstruments, QueryBuilder)
{
    EXPECT_EQ(bitget::instruments_query("USDT-FUTURES", "BTCUSDT"),
              "category=USDT-FUTURES&symbol=BTCUSDT");
    EXPECT_EQ(bitget::instruments_query("USDT-FUTURES", ""),
              "category=USDT-FUTURES");
}

TEST(BitgetInstruments, ParseOkTrading)
{
    auto p = bitget::parse_instruments_response(kInstrumentsBtcOk, "BTCUSDT");
    EXPECT_TRUE(p.ok);
    EXPECT_TRUE(p.found);
    EXPECT_TRUE(p.trading);
    EXPECT_EQ(p.spec.symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(p.spec.tick_size, 0.1);
    EXPECT_DOUBLE_EQ(p.spec.lot_size, 0.001);
    EXPECT_DOUBLE_EQ(p.spec.min_qty, 0.001);
    EXPECT_DOUBLE_EQ(p.spec.min_notional, 5.0);
    EXPECT_DOUBLE_EQ(p.spec.maker_rate, 0.0002);
    EXPECT_DOUBLE_EQ(p.spec.taker_rate, 0.0006);
}

TEST(BitgetInstruments, ParseSymbolMissing)
{
    auto p = bitget::parse_instruments_response(kInstrumentsBtcOk, "ETHUSDT");
    EXPECT_FALSE(p.ok);
    EXPECT_FALSE(p.found);
    EXPECT_NE(p.note.find("not found"), std::string::npos);
}

TEST(BitgetInstruments, ParseOfflineNotTrading)
{
    auto p = bitget::parse_instruments_response(kInstrumentsOffline, "BTCUSDT");
    EXPECT_FALSE(p.ok);
    EXPECT_TRUE(p.found);
    EXPECT_FALSE(p.trading);
    EXPECT_EQ(p.status, "offline");
}

TEST(BitgetInstruments, ParseBusinessError)
{
    auto p = bitget::parse_instruments_response(kInstrumentsBusinessFail, "BTCUSDT");
    EXPECT_FALSE(p.ok);
    EXPECT_FALSE(p.found);
    EXPECT_NE(p.note.find("40001"), std::string::npos);
}

TEST(BitgetInstruments, ParseEmptyData)
{
    auto p = bitget::parse_instruments_response(
        R"({"code":"00000","data":[]})", "BTCUSDT");
    EXPECT_FALSE(p.ok);
    EXPECT_FALSE(p.found);
}

#endif // HAS_BITGET
