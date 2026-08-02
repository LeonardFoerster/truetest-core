#include <gtest/gtest.h>

#ifdef HAS_BITUNIX

#include "providers/bitunix/bitunix_auth.h"

#include <string>

TEST(BitunixAuth, Sha256HexKnownVector)
{
    // SHA256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    auto hex = bitunix::sha256_hex("");
    EXPECT_EQ(hex,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(BitunixAuth, SortedQueryConcatOrdersKeys)
{
    std::vector<std::pair<std::string, std::string>> params = {
        {"uid", "200"},
        {"id", "1"},
    };
    EXPECT_EQ(bitunix::sorted_query_concat(params), "id1uid200");
}

TEST(BitunixAuth, SortedQueryFromString)
{
    EXPECT_EQ(bitunix::sorted_query_from_string("uid=200&id=1"), "id1uid200");
    EXPECT_EQ(bitunix::sorted_query_from_string(""), "");
}

TEST(BitunixAuth, DoubleSha256Deterministic)
{
    // Fixed inputs → stable sign (not a venue golden; verifies composition).
    const auto s1 = bitunix::sign_double_sha256(
        "nonce1", "1700000000000", "apikey", "id1", "{}", "secret");
    const auto s2 = bitunix::sign_double_sha256(
        "nonce1", "1700000000000", "apikey", "id1", "{}", "secret");
    EXPECT_FALSE(s1.empty());
    EXPECT_EQ(s1.size(), 64u);
    EXPECT_EQ(s1, s2);

    const auto different = bitunix::sign_double_sha256(
        "nonce2", "1700000000000", "apikey", "id1", "{}", "secret");
    EXPECT_NE(s1, different);
}

TEST(BitunixAuth, MakeNonceHexWidth)
{
    auto n = bitunix::make_nonce_hex(0xabc);
    EXPECT_EQ(n.size(), 32u);
    EXPECT_EQ(n.back(), 'c');
}

#else

TEST(BitunixAuth, SkippedWithoutHasBitunix)
{
    GTEST_SKIP() << "HAS_BITUNIX not defined";
}

#endif // HAS_BITUNIX
