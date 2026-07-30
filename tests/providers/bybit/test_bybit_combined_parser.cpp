#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_combined_parser.h"
#include "providers/bybit/bybit_transport.h"

#include <string>
#include <variant>

namespace {

constexpr const char* kTrade = R"({
  "topic": "publicTrade.BTCUSDT",
  "type": "snapshot",
  "ts": 1002,
  "data": [
    {"T":1000,"s":"BTCUSDT","S":"Buy","v":"0.1","p":"100.0","i":"1"},
    {"T":1001,"s":"BTCUSDT","S":"Sell","v":"0.2","p":"101.0","i":"2"}
  ]
})";

constexpr const char* kOb = R"({
  "topic": "orderbook.50.BTCUSDT",
  "type": "snapshot",
  "ts": 1,
  "data": {
    "s": "BTCUSDT",
    "b": [["100.0","1.0"]],
    "a": [["101.0","2.0"]]
  },
  "cts": 1
})";

constexpr const char* kKlineClosed = R"({
  "topic": "kline.1.BTCUSDT",
  "type": "snapshot",
  "ts": 1,
  "data": [{
    "start": 1000,
    "open": "10",
    "high": "12",
    "low": "9",
    "close": "11",
    "volume": "5",
    "confirm": true
  }]
})";

} // namespace

TEST(BybitCombinedParser, RoutesTradeMultiEmit)
{
    BybitCombinedParser p;
    auto batch = p.parse_records(kTrade);
    ASSERT_EQ(batch.size(), 2u);
    ASSERT_TRUE(std::holds_alternative<provider::tick>(batch[0]));
    ASSERT_TRUE(std::holds_alternative<provider::tick>(batch[1]));
    EXPECT_DOUBLE_EQ(std::get<provider::tick>(batch[0]).price, 100.0);
    EXPECT_DOUBLE_EQ(std::get<provider::tick>(batch[1]).price, 101.0);
}

TEST(BybitCombinedParser, RoutesOrderbook)
{
    BybitCombinedParser p;
    auto ev = p.parse_record(std::string_view{kOb});
    ASSERT_TRUE(ev.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::l2_snapshot>(*ev));
    auto& snap = std::get<provider::l2_snapshot>(*ev);
    EXPECT_EQ(snap.symbol, "BTCUSDT");
    ASSERT_EQ(snap.bids.size(), 1u);
}

TEST(BybitCombinedParser, RoutesClosedKline)
{
    BybitCombinedParser p;
    auto batch = p.parse_records(kKlineClosed);
    ASSERT_EQ(batch.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<provider::bar>(batch[0]));
    EXPECT_DOUBLE_EQ(std::get<provider::bar>(batch[0]).close, 11.0);
}

TEST(BybitCombinedParser, ParseRecordReturnsFirstTrade)
{
    BybitCombinedParser p;
    auto ev = p.parse_record(std::string_view{kTrade});
    ASSERT_TRUE(ev.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::tick>(*ev));
    EXPECT_DOUBLE_EQ(std::get<provider::tick>(*ev).price, 100.0);
}

// --- Subscribe JSON helpers (transport pure functions) ---

TEST(BybitTransportSubscribe, MapTradeToPublicTrade)
{
    auto m = bybit::map_stream_to_topic("trade");
    EXPECT_EQ(m.topic, "publicTrade");
}

TEST(BybitTransportSubscribe, MapOrderbookDepth)
{
    auto m = bybit::map_stream_to_topic("orderbook.50");
    EXPECT_EQ(m.topic, "orderbook.50");
    auto m2 = bybit::map_stream_to_topic("books50");
    EXPECT_EQ(m2.topic, "orderbook.50");
}

TEST(BybitTransportSubscribe, MapKlineInterval)
{
    auto m1 = bybit::map_stream_to_topic("kline1m");
    EXPECT_EQ(m1.topic, "kline.1");
    auto m5 = bybit::map_stream_to_topic("kline.5");
    EXPECT_EQ(m5.topic, "kline.5");
    auto mh = bybit::map_stream_to_topic("kline1h");
    EXPECT_EQ(mh.topic, "kline.60");
    auto md = bybit::map_stream_to_topic("kline1d");
    EXPECT_EQ(md.topic, "kline.D");
}

TEST(BybitTransportSubscribe, SubscribeJsonTradeExact)
{
    const std::string j =
        bybit::build_subscribe_json("BTCUSDT", "publicTrade");
    EXPECT_EQ(j, R"({"op":"subscribe","args":["publicTrade.BTCUSDT"]})");
}

TEST(BybitTransportSubscribe, SubscribeJsonMulti)
{
    std::vector<std::string> streams{"trade", "orderbook.50"};
    const std::string j =
        bybit::build_subscribe_json_for_streams("BTCUSDT", streams);
    EXPECT_EQ(
        j,
        R"({"op":"subscribe","args":["publicTrade.BTCUSDT","orderbook.50.BTCUSDT"]})");
}

TEST(BybitTransportSubscribe, PingJson)
{
    EXPECT_EQ(bybit::build_ping_json(), R"({"op":"ping"})");
}

TEST(BybitTransportSubscribe, PongAndAckDetection)
{
    EXPECT_TRUE(bybit::is_pong_frame(R"({"op":"pong","args":["1760000000"]})"));
    EXPECT_TRUE(bybit::is_control_ack(
        R"({"success":true,"ret_msg":"subscribe","op":"subscribe"})"));
    EXPECT_FALSE(bybit::is_pong_frame(kTrade));
}

#endif // HAS_BYBIT
