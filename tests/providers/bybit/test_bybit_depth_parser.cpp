#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_depth_parser.h"

#include <chrono>

namespace {

constexpr const char* kSnapshot = R"({
  "topic": "orderbook.50.BTCUSDT",
  "type": "snapshot",
  "ts": 1672304484978,
  "data": {
    "s": "BTCUSDT",
    "b": [
      ["16493.50", "0.006"],
      ["16493.00", "0.100"]
    ],
    "a": [
      ["16611.00", "0.029"],
      ["16612.00", "0.213"]
    ],
    "u": 18521288,
    "seq": 7961638724
  },
  "cts": 1672304484976
})";

constexpr const char* kDeltaDelete = R"({
  "topic": "orderbook.50.BTCUSDT",
  "type": "delta",
  "ts": 1672304484978,
  "data": {
    "s": "BTCUSDT",
    "b": [
      ["16493.50", "0"]
    ],
    "a": [
      ["16611.00", "0.050"]
    ],
    "u": 18521289,
    "seq": 7961638725
  },
  "cts": 1672304484977
})";

} // namespace

TEST(BybitDepthParser, SnapshotFieldsAndLevels)
{
    auto snap = bybit::parse_orderbook(kSnapshot);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->symbol, "BTCUSDT");
    ASSERT_EQ(snap->bids.size(), 2u);
    ASSERT_EQ(snap->asks.size(), 2u);
    EXPECT_DOUBLE_EQ(snap->bids[0].price, 16493.50);
    EXPECT_EQ(snap->bids[0].quantity, static_cast<int64_t>(0.006 * 1e8));
    EXPECT_DOUBLE_EQ(snap->asks[0].price, 16611.00);
    EXPECT_EQ(snap->asks[0].quantity, static_cast<int64_t>(0.029 * 1e8));

    auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     snap->timestamp.time_since_epoch())
                     .count();
    // Prefer cts over ts.
    EXPECT_EQ(ts_ms, 1672304484976LL);
}

TEST(BybitDepthParser, DeltaSizeZeroIsDelete)
{
    auto snap = bybit::parse_orderbook(kDeltaDelete);
    ASSERT_TRUE(snap.has_value());
    ASSERT_EQ(snap->bids.size(), 1u);
    EXPECT_DOUBLE_EQ(snap->bids[0].price, 16493.50);
    EXPECT_EQ(snap->bids[0].quantity, 0); // delete
    ASSERT_EQ(snap->asks.size(), 1u);
    EXPECT_EQ(snap->asks[0].quantity, static_cast<int64_t>(0.050 * 1e8));
}

TEST(BybitDepthParser, RejectsTradeTopic)
{
    constexpr const char* kTrade = R"({
      "topic":"publicTrade.BTCUSDT",
      "data":[{"p":"1","v":"1","S":"Buy","T":1,"s":"BTCUSDT"}]
    })";
    EXPECT_FALSE(bybit::parse_orderbook(kTrade).has_value());
}

TEST(BybitDepthParser, DepthParserAdapter)
{
    BybitDepthParser parser;
    auto snap = parser.parse_record(std::string_view{kSnapshot});
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->symbol, "BTCUSDT");
}

TEST(BybitDepthParser, ParseOrderbookUpdates)
{
    auto ups = bybit::parse_orderbook_updates(kSnapshot);
    // 2 bids + 2 asks
    ASSERT_EQ(ups.size(), 4u);
    EXPECT_EQ(ups[0].side, 0);
    EXPECT_EQ(ups[2].side, 1);
}

#endif // HAS_BYBIT
