#include <gtest/gtest.h>

#ifdef HAS_GATE

#include "providers/gate/gate_auth.h"

// Golden vectors: secret=`gate_secret`, openssl dgst -sha512 -hmac.

namespace {

constexpr const char* kSecret = "gate_secret";
constexpr const char* kTs     = "1710000000";

// SHA512("") known empty digest (split for readability in gate_auth.h comments)
constexpr const char* kEmptyBodyHash =
    "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
    "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e";

// HMAC-SHA512 hex of:
// GET\n/api/v4/futures/usdt/contracts/BTC_USDT\n\n<emptyBodyHash>\n1710000000
constexpr const char* kGoldenRest =
    "53995218787a747129514f71ef7707237b20ed1354be1c935d3b4daafc510bda"
    "5bd65985b5014969e55681b1fa6410c2ee4bd326a2f0a9f51e8935520f1779be";

// HMAC-SHA512 hex of channel=futures.orders&event=subscribe&time=1710000000
constexpr const char* kGoldenWs =
    "69ae3b9a560db41e5dac9a51e1a35843af74920f052c9d347e6fb5c68d31df68"
    "2f3273fccac81eb4000f3ec8ea0868e37c908fdb615fc7902c7b589437ff0866";

} // namespace

TEST(GateAuth, Sha512EmptyBodyKnownDigest)
{
    EXPECT_EQ(gate::sha512_hex(""), kEmptyBodyHash);
}

TEST(GateAuth, BuildRestSignStringShape)
{
    const auto s = gate::build_rest_sign_string(
        "GET",
        "/api/v4/futures/usdt/contracts/BTC_USDT",
        "",
        "",
        kTs);
    EXPECT_EQ(s,
              std::string("GET\n")
                  + "/api/v4/futures/usdt/contracts/BTC_USDT\n"
                  + "\n"
                  + kEmptyBodyHash + "\n"
                  + kTs);
}

TEST(GateAuth, SignRestGoldenHex)
{
    const auto sig = gate::sign_rest(
        kSecret,
        "GET",
        "/api/v4/futures/usdt/contracts/BTC_USDT",
        "",
        "",
        kTs);
    EXPECT_EQ(sig, kGoldenRest);
}

TEST(GateAuth, BuildWsSignString)
{
    EXPECT_EQ(gate::build_ws_sign_string("futures.orders", "subscribe", kTs),
              "channel=futures.orders&event=subscribe&time=1710000000");
}

TEST(GateAuth, SignWsGoldenHex)
{
    const auto sig = gate::sign_ws(
        kSecret, "futures.orders", "subscribe", kTs);
    EXPECT_EQ(sig, kGoldenWs);
}

TEST(GateAuth, RedactScrubsKey)
{
    const auto red = gate::redact_for_log(R"("KEY":"supersecret")");
    EXPECT_NE(red.find("<redacted>"), std::string::npos);
    EXPECT_EQ(red.find("supersecret"), std::string::npos);
}

#endif // HAS_GATE
