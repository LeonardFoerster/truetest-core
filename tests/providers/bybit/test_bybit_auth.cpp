#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_auth.h"

// Golden vectors: secret=`bybit_secret`, computed via openssl dgst -sha256 -hmac.
// REST prehash = timestamp + api_key + recv_window + payload
// WS prehash   = "GET/realtime" + expires_ms

namespace {

constexpr const char* kSecret = "bybit_secret";
constexpr const char* kKey    = "test_api_key";
constexpr const char* kTs     = "1658385579423";
constexpr const char* kRecv   = "5000";
constexpr const char* kQuery  = "category=linear&symbol=BTCUSDT";

// HMAC-SHA256 hex of prehash:
// 1658385579423test_api_key5000category=linear&symbol=BTCUSDT
constexpr const char* kGoldenRest =
    "b2197ec3bbe623288c6ffdc2faf555ca13af7ad5e4081c333e0e83d6cd5cf4a1";

// HMAC-SHA256 hex of "GET/realtime1658385579423"
constexpr const char* kGoldenWs =
    "328b6383677d3e16e25e53ba954256c548b7209b04c3a93afd68a2efa685ce1b";

} // namespace

TEST(BybitAuth, BuildRestPrehashConcat)
{
    const auto pre = bybit::build_rest_prehash(kTs, kKey, kRecv, kQuery);
    EXPECT_EQ(pre, "1658385579423test_api_key5000category=linear&symbol=BTCUSDT");
}

TEST(BybitAuth, BuildRestPrehashEmptyPayload)
{
    const auto pre = bybit::build_rest_prehash(kTs, kKey, kRecv, "");
    EXPECT_EQ(pre, "1658385579423test_api_key5000");
}

TEST(BybitAuth, SignRestGoldenHex)
{
    const auto sig = bybit::sign_rest(kSecret, kTs, kKey, kRecv, kQuery);
    EXPECT_EQ(sig, kGoldenRest);
}

TEST(BybitAuth, SignWsAuthGoldenHex)
{
    const auto sig = bybit::sign_ws_auth(kSecret, kTs);
    EXPECT_EQ(sig, kGoldenWs);
}

TEST(BybitAuth, BuildWsAuthPrehash)
{
    EXPECT_EQ(bybit::build_ws_auth_prehash("12345"), "GET/realtime12345");
}

TEST(BybitAuth, RedactScrubsApiKeyHeader)
{
    const auto red = bybit::redact_for_log(
        R"(X-BAPI-API-KEY: supersecretkey123 body)");
    EXPECT_NE(red.find("<redacted>"), std::string::npos);
    EXPECT_EQ(red.find("supersecretkey123"), std::string::npos);
}

#endif // HAS_BYBIT
