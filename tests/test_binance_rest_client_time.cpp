#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_rest_client.h"
#include "providers/binance/binance_time_sync.h"

#include <climits>
#include <functional>
#include <string>

namespace {

using response = BinanceRestClient::response;
using get_fn_t = std::function<response(const std::string&, const std::string&)>;

get_fn_t make_ok(long long server_ms, int status = 200)
{
    return [server_ms, status](const std::string&, const std::string&) {
        response r;
        r.status = status;
        r.body = std::string("{\"serverTime\":") + std::to_string(server_ms)
                 + "}";
        return r;
    };
}

} // namespace

TEST(BinanceClockSkew, OffsetFailsOnNetworkError)
{
    auto fn = [](const std::string&, const std::string&) {
        return response{0, "", 0};
    };
    EXPECT_EQ(BinanceRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(BinanceClockSkew, OffsetFailsOnHttp4xx)
{
    auto fn = make_ok(0, 403);
    EXPECT_EQ(BinanceRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(BinanceClockSkew, OffsetFailsOnMalformedBody)
{
    auto fn = [](const std::string&, const std::string&) {
        response r{200, "{\"unrelated\":42}", 0};
        return r;
    };
    EXPECT_EQ(BinanceRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(BinanceClockSkew, OffsetNullCallableReturnsSentinel)
{
    get_fn_t empty;
    EXPECT_EQ(BinanceRestClient::server_time_offset_ms(empty), LLONG_MIN);
}

TEST(BinanceClockSkew, VerifyOkWithinTolerance)
{
    auto r = binance::verify_clock_skew_offset(500, 2000);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.offset_ms, 500);
}

TEST(BinanceClockSkew, VerifyOkAtBoundary)
{
    auto r = binance::verify_clock_skew_offset(-2000, 2000);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.offset_ms, -2000);
}

TEST(BinanceClockSkew, VerifyFailsPositiveDrift)
{
    auto r = binance::verify_clock_skew_offset(10'000, 2000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("drift"), std::string::npos);
}

TEST(BinanceClockSkew, VerifyFailsNegativeDrift)
{
    auto r = binance::verify_clock_skew_offset(-10'000, 2000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("drift"), std::string::npos);
}

TEST(BinanceClockSkew, VerifyReportsFetchFailure)
{
    auto r = binance::verify_clock_skew_offset(LLONG_MIN, 2000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("fetch"), std::string::npos);
}

#endif // HAS_BINANCE
