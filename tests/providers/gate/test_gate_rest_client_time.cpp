#include <gtest/gtest.h>

#ifdef HAS_GATE

#include "providers/gate/gate_auth.h"
#include "providers/gate/gate_endpoints.h"
#include "providers/gate/gate_rest_client.h"
#include "providers/gate/gate_time_sync.h"

#include <climits>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>

namespace {

using response = GateRestClient::response;
using get_fn_t =
    std::function<response(const std::string&, const std::string&)>;

// Gate GET /api/v4/spot/time body (server_time in ms, as live docs).
get_fn_t make_time_ok(long long server_ms, int status = 200)
{
    return [server_ms, status](const std::string&, const std::string&) {
        response r;
        r.status = status;
        r.body = std::string("{\"server_time\":") + std::to_string(server_ms)
                 + "}";
        r.business_ok = gate::is_http_success(status);
        return r;
    };
}

// Minimal futures contract object matching live field names.
constexpr const char* kContractBtcOk =
    R"({"name":"BTC_USDT","type":"direct","quanto_multiplier":"0.0001",)"
    R"("order_price_round":"0.1","mark_price_round":"0.01",)"
    R"("order_size_min":1,"order_size_max":1000000,"order_size_step":1,)"
    R"("maker_fee_rate":"-0.00015","taker_fee_rate":"0.0005",)"
    R"("in_delisting":false,"status":"trading"})";

constexpr const char* kContractDelisting =
    R"({"name":"BTC_USDT","quanto_multiplier":"0.0001",)"
    R"("order_price_round":"0.1","order_size_min":1,"in_delisting":true})";

constexpr const char* kContractError =
    R"({"label":"CONTRACT_NOT_FOUND","message":"Contract not found"})";

} // namespace

// --- path helpers (signing uses full /api/v4 path) ---------------------------

TEST(GateRestPaths, SpotTimeIncludesApiV4)
{
    auto ep = gate::usdt_mainnet();
    EXPECT_EQ(gate::time_path(ep), "/api/v4/spot/time");
    EXPECT_EQ(gate::spot_time_path(ep), "/api/v4/spot/time");
}

TEST(GateRestPaths, ContractPathIncludesApiV4AndSettle)
{
    auto ep = gate::usdt_mainnet();
    EXPECT_EQ(gate::contract_path(ep, "BTC_USDT"),
              "/api/v4/futures/usdt/contracts/BTC_USDT");
    EXPECT_EQ(gate::futures_sign_path(ep, "/orders"),
              "/api/v4/futures/usdt/orders");
}

TEST(GateRestPaths, TestnetHosts)
{
    auto ep = gate::usdt_testnet();
    EXPECT_TRUE(ep.is_testnet);
    EXPECT_EQ(gate::time_path(ep), "/api/v4/spot/time");
    EXPECT_EQ(gate::contract_path(ep, "ETH_USDT"),
              "/api/v4/futures/usdt/contracts/ETH_USDT");
}

// --- timestamp unit helpers --------------------------------------------------

TEST(GateRestTimestamp, SecondsStringFromMs)
{
    EXPECT_EQ(gate::timestamp_seconds_string(1'710'000'000'123LL),
              "1710000000");
    EXPECT_EQ(gate::timestamp_seconds_string(999), "0");
}

TEST(GateRestTimestamp, SignUsesSecondsNotMs)
{
    // Guard the unit mixup pitfall: SIGN string ends with seconds.
    const auto s = gate::build_rest_sign_string(
        "GET",
        "/api/v4/futures/usdt/contracts/BTC_USDT",
        "",
        "",
        gate::timestamp_seconds_string(1'710'000'000'500LL));
    EXPECT_NE(s.find("\n1710000000"), std::string::npos);
    EXPECT_EQ(s.find("1710000000500"), std::string::npos);
}

// --- server time parse -------------------------------------------------------

TEST(GateServerTimeParse, NumericServerTimeMs)
{
    long long ms = 0;
    const char* body = R"({"server_time":1688008631614})";
    ASSERT_TRUE(gate::parse_server_time_ms(body, ms));
    EXPECT_EQ(ms, 1688008631614LL);
}

TEST(GateServerTimeParse, StringServerTimeMs)
{
    long long ms = 0;
    const char* body = R"({"server_time":"1710000000123"})";
    ASSERT_TRUE(gate::parse_server_time_ms(body, ms));
    EXPECT_EQ(ms, 1710000000123LL);
}

TEST(GateServerTimeParse, SecondsPromotedToMs)
{
    // Soft guard: if venue ever returns seconds, promote.
    long long ms = 0;
    const char* body = R"({"server_time":1710000000})";
    ASSERT_TRUE(gate::parse_server_time_ms(body, ms));
    EXPECT_EQ(ms, 1710000000000LL);
}

TEST(GateServerTimeParse, MalformedFails)
{
    long long ms = 0;
    EXPECT_FALSE(gate::parse_server_time_ms(R"({})", ms));
    EXPECT_FALSE(gate::parse_server_time_ms(
        R"({"server_time":"not-a-number"})", ms));
}

// --- server_time_offset_ms via injectable get_fn -----------------------------

TEST(GateClockSkew, OffsetFailsOnNetworkError)
{
    auto fn = [](const std::string&, const std::string&) {
        return response{0, "", false};
    };
    EXPECT_EQ(GateRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(GateClockSkew, OffsetFailsOnHttp4xx)
{
    auto fn = make_time_ok(0, 403);
    EXPECT_EQ(GateRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(GateClockSkew, OffsetFailsOnMalformedBody)
{
    auto fn = [](const std::string&, const std::string&) {
        return response{200, R"({"unrelated":42})", true};
    };
    EXPECT_EQ(GateRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(GateClockSkew, OffsetNullCallableReturnsSentinel)
{
    get_fn_t empty;
    EXPECT_EQ(GateRestClient::server_time_offset_ms(empty), LLONG_MIN);
}

TEST(GateClockSkew, OffsetNearZeroForCurrentServerTime)
{
    const long long now = static_cast<long long>(gate::local_time_ms());
    auto fn = make_time_ok(now);
    auto off = GateRestClient::server_time_offset_ms(fn);
    ASSERT_NE(off, LLONG_MIN);
    EXPECT_LT(std::llabs(off), 50);
}

TEST(GateClockSkew, OffsetUsesDefaultTimePath)
{
    std::string seen_path;
    auto fn = [&](const std::string& path, const std::string&) {
        seen_path = path;
        response r;
        r.status = 200;
        r.body = std::string("{\"server_time\":")
                 + std::to_string(gate::local_time_ms()) + "}";
        r.business_ok = true;
        return r;
    };
    (void)GateRestClient::server_time_offset_ms(fn);
    EXPECT_EQ(seen_path, "/api/v4/spot/time");
}

// --- verify_clock_skew pure logic --------------------------------------------

TEST(GateClockSkew, VerifyOkWithinTolerance)
{
    auto r = gate::verify_clock_skew_offset(500, 2000);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.offset_ms, 500);
}

TEST(GateClockSkew, VerifyOkAtBoundary)
{
    auto r = gate::verify_clock_skew_offset(-2000, 2000);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.offset_ms, -2000);
}

TEST(GateClockSkew, VerifyFailsPositiveDrift)
{
    auto r = gate::verify_clock_skew_offset(10'000, 2000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("drift"), std::string::npos);
}

TEST(GateClockSkew, VerifyFailsNegativeDrift)
{
    auto r = gate::verify_clock_skew_offset(-10'000, 2000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("drift"), std::string::npos);
}

TEST(GateClockSkew, VerifyReportsFetchFailure)
{
    auto r = gate::verify_clock_skew_offset(LLONG_MIN, 2000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("fetch"), std::string::npos);
}

// --- lazy-resync decision ----------------------------------------------------

TEST(GateClockResyncDue, NeverSyncedReturnsTrue)
{
    EXPECT_TRUE(GateRestClient::resync_due(0, 0, 300'000));
    EXPECT_TRUE(GateRestClient::resync_due(1'000'000, 0, 300'000));
    EXPECT_TRUE(GateRestClient::resync_due(1'000'000, -1, 300'000));
}

TEST(GateClockResyncDue, WithinIntervalReturnsFalse)
{
    EXPECT_FALSE(GateRestClient::resync_due(360'000, 300'000, 300'000));
}

TEST(GateClockResyncDue, AtIntervalBoundaryIsDue)
{
    EXPECT_TRUE(GateRestClient::resync_due(600'000, 300'000, 300'000));
}

TEST(GateClockResyncDue, NonPositiveIntervalDisablesLazySync)
{
    EXPECT_FALSE(GateRestClient::resync_due(10'000'000, 1, 0));
    EXPECT_FALSE(GateRestClient::resync_due(10'000'000, 1, -5));
}

// --- contract probe (canned JSON) --------------------------------------------

TEST(GateContractProbe, ParseOkTrading)
{
    auto p = gate::parse_contract_response(kContractBtcOk, "BTC_USDT");
    EXPECT_TRUE(p.ok);
    EXPECT_TRUE(p.found);
    EXPECT_TRUE(p.trading);
    EXPECT_EQ(p.spec.symbol, "BTC_USDT");
    EXPECT_DOUBLE_EQ(p.spec.tick_size, 0.1);
    EXPECT_DOUBLE_EQ(p.spec.lot_size, 1.0);
    EXPECT_DOUBLE_EQ(p.spec.min_qty, 1.0);
    EXPECT_DOUBLE_EQ(p.quanto_multiplier, 0.0001);
    EXPECT_DOUBLE_EQ(p.spec.maker_rate, -0.00015);
    EXPECT_DOUBLE_EQ(p.spec.taker_rate, 0.0005);
}

TEST(GateContractProbe, ParseDefaultLotWhenStepMissing)
{
    const char* body =
        R"({"name":"ETH_USDT","quanto_multiplier":"0.01",)"
        R"("order_price_round":"0.01","order_size_min":1,)"
        R"("in_delisting":false})";
    auto p = gate::parse_contract_response(body, "ETH_USDT");
    EXPECT_TRUE(p.ok);
    EXPECT_DOUBLE_EQ(p.spec.lot_size, 1.0);
    EXPECT_DOUBLE_EQ(p.quanto_multiplier, 0.01);
}

TEST(GateContractProbe, ParseDelistingNotTrading)
{
    auto p = gate::parse_contract_response(kContractDelisting, "BTC_USDT");
    EXPECT_FALSE(p.ok);
    EXPECT_TRUE(p.found);
    EXPECT_FALSE(p.trading);
}

TEST(GateContractProbe, ParseErrorLabel)
{
    auto p = gate::parse_contract_response(kContractError, "BTC_USDT");
    EXPECT_FALSE(p.ok);
    EXPECT_FALSE(p.found);
    EXPECT_NE(p.note.find("CONTRACT_NOT_FOUND"), std::string::npos);
}

TEST(GateContractProbe, ParseMissingQuanto)
{
    const char* body =
        R"({"name":"BTC_USDT","order_price_round":"0.1","order_size_min":1})";
    auto p = gate::parse_contract_response(body, "BTC_USDT");
    EXPECT_FALSE(p.ok);
    EXPECT_NE(p.note.find("quanto"), std::string::npos);
}

TEST(GateContractProbe, NameMismatch)
{
    auto p = gate::parse_contract_response(kContractBtcOk, "ETH_USDT");
    EXPECT_FALSE(p.ok);
    EXPECT_TRUE(p.found);
    EXPECT_NE(p.note.find("mismatch"), std::string::npos);
}

TEST(GateContractProbe, NotionalUsdt)
{
    // 10 contracts * 0.0001 BTC/contract * 50000 = 50 USDT
    EXPECT_DOUBLE_EQ(gate::notional_usdt(10.0, 50'000.0, 0.0001), 50.0);
    EXPECT_DOUBLE_EQ(gate::notional_usdt(-10.0, 50'000.0, 0.0001), 50.0);
}

TEST(GateHttpSuccess, StatusWindow)
{
    EXPECT_TRUE(gate::is_http_success(200));
    EXPECT_TRUE(gate::is_http_success(204));
    EXPECT_FALSE(gate::is_http_success(199));
    EXPECT_FALSE(gate::is_http_success(400));
    EXPECT_FALSE(gate::is_http_success(0));
}

// --- sign-path prehash still matches Phase 0 golden --------------------------

TEST(GateRestSign, PathWithApiV4InPrehash)
{
    const auto sig = gate::sign_rest(
        "gate_secret",
        "GET",
        "/api/v4/futures/usdt/contracts/BTC_USDT",
        "",
        "",
        "1710000000");
    // Same golden as test_gate_auth (path must include /api/v4).
    EXPECT_EQ(sig,
              "53995218787a747129514f71ef7707237b20ed1354be1c935d3b4daafc510bda"
              "5bd65985b5014969e55681b1fa6410c2ee4bd326a2f0a9f51e8935520f1779be");
}

#endif // HAS_GATE
