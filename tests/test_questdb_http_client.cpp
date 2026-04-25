#ifdef HAS_QUESTDB

#include "data/questdb/http_client.h"

#include <gtest/gtest.h>

using truetest::questdb::url_encode;

TEST(QuestdbHttpClient, UrlEncodeBasic)
{
    EXPECT_EQ(url_encode(" "), "%20");
    EXPECT_EQ(url_encode("="), "%3D");
    EXPECT_EQ(url_encode("&"), "%26");
    EXPECT_EQ(url_encode("?"), "%3F");
}

TEST(QuestdbHttpClient, UrlEncodeUnreservedPassesThrough)
{
    const std::string s = "abcXYZ012-_.~";
    EXPECT_EQ(url_encode(s), s);
}

TEST(QuestdbHttpClient, UrlEncodeSqlCreateTable)
{
    const std::string sql = "CREATE TABLE foo (a INT)";
    // Spaces -> %20, parens -> %28 / %29
    EXPECT_EQ(url_encode(sql),
              "CREATE%20TABLE%20foo%20%28a%20INT%29");
}

TEST(QuestdbHttpClient, UrlEncodeEmpty)
{
    EXPECT_EQ(url_encode(""), "");
}

TEST(QuestdbHttpClient, UrlEncodeNonAscii)
{
    // 'ü' is UTF-8 0xC3 0xBC.
    EXPECT_EQ(url_encode("\xC3\xBC"), "%C3%BC");
}

#endif // HAS_QUESTDB
