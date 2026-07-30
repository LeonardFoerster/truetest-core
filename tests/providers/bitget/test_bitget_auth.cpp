#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_auth.h"

// Golden vectors: secret=`bitget_secret`, computed offline via
//   printf '%s' '<prehash>' | openssl dgst -sha256 -hmac 'bitget_secret' -binary | base64 -w0
// Prehash shapes match Bitget UTA v3 REST / WS login docs.

namespace {

constexpr const char* kSecret = "bitget_secret";
constexpr const char* kTs = "1627366780545";

// Base64(HMAC-SHA256(secret, "1627366780545GET/api/v3/account/assets"))
constexpr const char* kGoldenGetAssets =
    "oRC2tj1pFkWAp8ZR0/oISVKUKhUZELuXDfxSK5oABOQ=";

// Base64(HMAC-SHA256(secret, "1627366780545GET/user/verify"))
constexpr const char* kGoldenWsLogin =
    "BsKPjD3dd8QMqNVXiQvXERNK4rH2ckyjbnX3Ra/wqNs=";

// Base64(HMAC-SHA256(secret,
//   "1627366780545POST/api/v3/trade/place-order{\"category\":\"USDT-FUTURES\"}"))
constexpr const char* kGoldenPostPlaceOrder =
    "u93SJEta4wfTR/CVwZbg6x2XG+8o2Cm9mNHtOdZpLqk=";

// Base64(HMAC-SHA256(secret, "1627366780545GET/api/v3/account/assets?coin=USDT"))
constexpr const char* kGoldenGetWithQuery =
    "q36nay1fFMpxaFJw2DnH7GLRlYB5ORygL9jq6JYzbnU=";

} // namespace

TEST(BitgetAuth, PrehashGetNoQueryNoBody)
{
    const auto pre = bitget::build_prehash(
        kTs, "GET", "/api/v3/account/assets", "", "");
    EXPECT_EQ(pre, "1627366780545GET/api/v3/account/assets");
}

TEST(BitgetAuth, PrehashPostWithBody)
{
    const auto body = R"({"category":"USDT-FUTURES"})";
    const auto pre = bitget::build_prehash(
        kTs, "POST", "/api/v3/trade/place-order", "", body);
    EXPECT_EQ(pre,
              "1627366780545POST/api/v3/trade/place-order"
              "{\"category\":\"USDT-FUTURES\"}");
}

TEST(BitgetAuth, PrehashGetWithQueryInsertsQuestionMark)
{
    const auto pre = bitget::build_prehash(
        kTs, "GET", "/api/v3/account/assets", "coin=USDT", "");
    EXPECT_EQ(pre, "1627366780545GET/api/v3/account/assets?coin=USDT");
}

TEST(BitgetAuth, SignRestGoldenBase64)
{
    const auto sig = bitget::sign_rest(
        kSecret, kTs, "GET", "/api/v3/account/assets", "", "");
    EXPECT_EQ(sig, kGoldenGetAssets);
}

TEST(BitgetAuth, SignRestPostBodyGoldenBase64)
{
    const auto body = R"({"category":"USDT-FUTURES"})";
    const auto sig = bitget::sign_rest(
        kSecret, kTs, "POST", "/api/v3/trade/place-order", "", body);
    EXPECT_EQ(sig, kGoldenPostPlaceOrder);
}

TEST(BitgetAuth, SignRestGetWithQueryGoldenBase64)
{
    const auto sig = bitget::sign_rest(
        kSecret, kTs, "GET", "/api/v3/account/assets", "coin=USDT", "");
    EXPECT_EQ(sig, kGoldenGetWithQuery);
}

TEST(BitgetAuth, SignWsLoginPrehashAndGolden)
{
    const auto pre = bitget::build_prehash(kTs, "GET", "/user/verify", "", "");
    EXPECT_EQ(pre, "1627366780545GET/user/verify");

    const auto sig = bitget::sign_ws_login(kSecret, kTs);
    EXPECT_EQ(sig, kGoldenWsLogin);
}

TEST(BitgetAuth, HmacSha256Base64SignerReusable)
{
    bitget::HmacSha256Base64Signer signer(kSecret);
    const auto a = signer.sign("1627366780545GET/api/v3/account/assets");
    const auto b = signer.sign("1627366780545GET/user/verify");
    EXPECT_EQ(a, kGoldenGetAssets);
    EXPECT_EQ(b, kGoldenWsLogin);
    // Second call with same input must match (context reset works).
    EXPECT_EQ(signer.sign("1627366780545GET/api/v3/account/assets"),
              kGoldenGetAssets);
}

TEST(BitgetAuth, EmptyKeySignerReturnsEmpty)
{
    bitget::HmacSha256Base64Signer signer("");
    EXPECT_TRUE(signer.sign("anything").empty());
}

#endif // HAS_BITGET
