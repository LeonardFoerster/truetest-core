#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_depth_parser.h"

#include <string>
#include <string_view>

TEST(BinanceDepthParser, PartialBookBidsAsks)
{
    const std::string json =
        R"({"lastUpdateId":12345,"bids":[["42000.0","1.5"],["41999.5","2.0"]],)"
        R"("asks":[["42001.0","0.8"],["42002.0","1.2"]]})";

    auto snap = binance::parse_depth_snapshot(json);
    ASSERT_TRUE(snap.has_value());
    ASSERT_EQ(snap->bids.size(), 2u);
    ASSERT_EQ(snap->asks.size(), 2u);
    EXPECT_DOUBLE_EQ(snap->bids[0].price, 42000.0);
    EXPECT_EQ(snap->bids[0].quantity, static_cast<int64_t>(1.5 * 1e8));
    EXPECT_DOUBLE_EQ(snap->asks[0].price, 42001.0);
    EXPECT_EQ(snap->asks[0].quantity, static_cast<int64_t>(0.8 * 1e8));
    EXPECT_TRUE(snap->symbol.empty()); // partial book has no "s"
}

TEST(BinanceDepthParser, DepthUpdateShortKeys)
{
    const std::string json =
        R"({"e":"depthUpdate","E":1704067200000,"s":"BTCUSDT",)"
        R"("U":1,"u":10,"b":[["42000.0","1.5"]],"a":[["42001.0","0.8"]]})";

    auto snap = binance::parse_depth_snapshot(std::string_view{json});
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->symbol, "BTCUSDT");
    ASSERT_EQ(snap->bids.size(), 1u);
    ASSERT_EQ(snap->asks.size(), 1u);
    EXPECT_DOUBLE_EQ(snap->bids[0].price, 42000.0);
    EXPECT_DOUBLE_EQ(snap->asks[0].price, 42001.0);

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  snap->timestamp.time_since_epoch())
                  .count();
    EXPECT_EQ(ms, 1704067200000LL);
}

TEST(BinanceDepthParser, DepthUpdatesExpandSides)
{
    const std::string json =
        R"({"e":"depthUpdate","E":1704067200000,"s":"ETHUSDT",)"
        R"("b":[["2000.0","1.0"],["1999.0","2.0"]],)"
        R"("a":[["2001.0","0.5"]]})";

    auto ups = binance::parse_depth_updates(json);
    ASSERT_EQ(ups.size(), 3u);

    EXPECT_EQ(ups[0].side, 0); // bid
    EXPECT_EQ(ups[0].symbol, "ETHUSDT");
    EXPECT_DOUBLE_EQ(ups[0].price, 2000.0);

    EXPECT_EQ(ups[1].side, 0);
    EXPECT_DOUBLE_EQ(ups[1].price, 1999.0);

    EXPECT_EQ(ups[2].side, 1); // ask
    EXPECT_EQ(ups[2].symbol, "ETHUSDT");
    EXPECT_DOUBLE_EQ(ups[2].price, 2001.0);
}

TEST(BinanceDepthParser, EmptyPayloadReturnsNullopt)
{
    EXPECT_FALSE(binance::parse_depth_snapshot(R"({"lastUpdateId":1})").has_value());
    EXPECT_TRUE(binance::parse_depth_updates(R"({"e":"depthUpdate","s":"X"})").empty());
}

TEST(BinanceDepthParser, Depth20TwentyLevels)
{
    std::string j = R"({"lastUpdateId":1,"bids":[)";
    for (int i = 0; i < 20; ++i)
    {
        if (i) j += ',';
        char buf[64];
        std::snprintf(buf, sizeof(buf), "[\"%.2f\",\"1.0\"]", 100.0 - i * 0.01);
        j += buf;
    }
    j += R"(],"asks":[)";
    for (int i = 0; i < 20; ++i)
    {
        if (i) j += ',';
        char buf[64];
        std::snprintf(buf, sizeof(buf), "[\"%.2f\",\"1.0\"]", 100.01 + i * 0.01);
        j += buf;
    }
    j += "]}";

    auto snap = binance::parse_depth_snapshot(j);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->bids.size(), 20u);
    EXPECT_EQ(snap->asks.size(), 20u);
    EXPECT_DOUBLE_EQ(snap->bids[0].price, 100.0);
    EXPECT_DOUBLE_EQ(snap->asks[19].price, 100.01 + 19 * 0.01);
}

TEST(BinanceDepthParser, StringViewOverloadNoCopy)
{
    // Ensure string_view path works without owning the buffer beyond the call.
    const char* raw =
        R"({"s":"BTCUSDT","E":1,"b":[["1.0","2.0"]],"a":[["3.0","4.0"]]})";
    auto snap = binance::parse_depth_snapshot(std::string_view{raw});
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(snap->bids[0].price, 1.0);
    EXPECT_DOUBLE_EQ(snap->asks[0].price, 3.0);
}

#endif // HAS_BINANCE
