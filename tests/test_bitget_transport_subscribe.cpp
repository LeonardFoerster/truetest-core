#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_transport.h"

#include <cerrno>
#include <string>
#include <vector>

// --- map_stream_to_topic (plan §7.5) ---

TEST(BitgetTransportSubscribe, MapTradeToPublicTrade)
{
    auto m = bitget::map_stream_to_topic("trade");
    EXPECT_EQ(m.topic, "publicTrade");
    EXPECT_TRUE(m.interval.empty());
}

TEST(BitgetTransportSubscribe, MapTickerIdentity)
{
    auto m = bitget::map_stream_to_topic("ticker");
    EXPECT_EQ(m.topic, "ticker");
    EXPECT_TRUE(m.interval.empty());
}

TEST(BitgetTransportSubscribe, MapDepthTopicsIdentity)
{
    for (const char* s : {"books1", "books5", "books50", "books"})
    {
        auto m = bitget::map_stream_to_topic(s);
        EXPECT_EQ(m.topic, s) << s;
        EXPECT_TRUE(m.interval.empty()) << s;
    }
}

TEST(BitgetTransportSubscribe, MapKlineInterval)
{
    auto m1 = bitget::map_stream_to_topic("kline1m");
    EXPECT_EQ(m1.topic, "kline");
    EXPECT_EQ(m1.interval, "1m");

    auto m5 = bitget::map_stream_to_topic("kline5m");
    EXPECT_EQ(m5.topic, "kline");
    EXPECT_EQ(m5.interval, "5m");

    auto m15 = bitget::map_stream_to_topic("kline15m");
    EXPECT_EQ(m15.topic, "kline");
    EXPECT_EQ(m15.interval, "15m");
}

TEST(BitgetTransportSubscribe, MapCandleAliasToKline)
{
    auto m = bitget::map_stream_to_topic("candle1m");
    EXPECT_EQ(m.topic, "kline");
    EXPECT_EQ(m.interval, "1m");

    auto m4 = bitget::map_stream_to_topic("candle4h");
    EXPECT_EQ(m4.topic, "kline");
    EXPECT_EQ(m4.interval, "4h");
}

TEST(BitgetTransportSubscribe, MapAlreadyMappedPassThrough)
{
    auto m = bitget::map_stream_to_topic("publicTrade");
    EXPECT_EQ(m.topic, "publicTrade");
    EXPECT_TRUE(m.interval.empty());
}

// --- subscribe JSON builders ---

TEST(BitgetTransportSubscribe, SubscribeJsonTradeExact)
{
    const std::string j =
        bitget::build_subscribe_json("BTCUSDT", "publicTrade");
    EXPECT_EQ(
        j,
        R"({"op":"subscribe","args":[{"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"}]})");
}

TEST(BitgetTransportSubscribe, SubscribeJsonBooks5Exact)
{
    const std::string j = bitget::build_subscribe_json("BTCUSDT", "books5");
    EXPECT_EQ(
        j,
        R"({"op":"subscribe","args":[{"instType":"usdt-futures","topic":"books5","symbol":"BTCUSDT"}]})");
}

TEST(BitgetTransportSubscribe, SubscribeJsonKlineWithInterval)
{
    auto m = bitget::map_stream_to_topic("kline1m");
    const std::string j =
        bitget::build_subscribe_json("ETHUSDT", m.topic, m.interval);
    EXPECT_EQ(
        j,
        R"({"op":"subscribe","args":[{"instType":"usdt-futures","topic":"kline","symbol":"ETHUSDT","interval":"1m"}]})");
}

TEST(BitgetTransportSubscribe, SubscribeJsonMultiTradeAndBooks5)
{
    const std::vector<std::string> streams = {"trade", "books5"};
    const std::string j =
        bitget::build_subscribe_json_for_streams("BTCUSDT", streams);
    EXPECT_EQ(
        j,
        R"({"op":"subscribe","args":[{"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},{"instType":"usdt-futures","topic":"books5","symbol":"BTCUSDT"}]})");
}

TEST(BitgetTransportSubscribe, SubscribeJsonMultiWithKline)
{
    std::vector<bitget::mapped_topic> topics = {
        bitget::map_stream_to_topic("trade"),
        bitget::map_stream_to_topic("kline1m"),
    };
    const std::string j = bitget::build_subscribe_json("BTCUSDT", topics);
    EXPECT_EQ(
        j,
        R"({"op":"subscribe","args":[{"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},{"instType":"usdt-futures","topic":"kline","symbol":"BTCUSDT","interval":"1m"}]})");
}

TEST(BitgetTransportSubscribe, InstTypeIsLowercaseUsdtFutures)
{
    const std::string j =
        bitget::build_subscribe_json("BTCUSDT", "publicTrade");
    EXPECT_NE(j.find("\"instType\":\"usdt-futures\""), std::string::npos);
    // Must not emit UPPER REST-style product type on WS.
    EXPECT_EQ(j.find("USDT-FUTURES"), std::string::npos);
}

TEST(BitgetTransportSubscribe, ToUpperAsciiSymbol)
{
    EXPECT_EQ(bitget::to_upper_ascii("btcusdt"), "BTCUSDT");
    EXPECT_EQ(bitget::to_upper_ascii("BTCUSDT"), "BTCUSDT");
}

TEST(BitgetTransportSubscribe, PingPongTextHelpers)
{
    EXPECT_TRUE(bitget::is_ping_text("ping"));
    EXPECT_TRUE(bitget::is_pong_text("pong"));
    EXPECT_FALSE(bitget::is_ping_text("pong"));
    EXPECT_FALSE(bitget::is_pong_text("ping"));
    EXPECT_FALSE(bitget::is_ping_text(R"({"op":"ping"})"));
}

TEST(BitgetTransportSubscribe, IsSocketRecvTimeoutRecognizesWakeErrors)
{
    using beast::error_code;
    namespace net = boost::asio;

    EXPECT_FALSE(bitget::is_socket_recv_timeout(error_code{}));
    EXPECT_TRUE(bitget::is_socket_recv_timeout(net::error::would_block));
    EXPECT_TRUE(bitget::is_socket_recv_timeout(net::error::try_again));
    EXPECT_TRUE(bitget::is_socket_recv_timeout(net::error::timed_out));
    EXPECT_TRUE(bitget::is_socket_recv_timeout(
        error_code(EAGAIN, boost::system::system_category())));
    EXPECT_TRUE(bitget::is_socket_recv_timeout(
        error_code(ETIMEDOUT, boost::system::system_category())));
    // Real disconnects must not be classified as wake.
    EXPECT_FALSE(bitget::is_socket_recv_timeout(
        boost::beast::websocket::error::closed));
}

#endif // HAS_BITGET
