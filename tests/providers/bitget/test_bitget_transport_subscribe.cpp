#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_transport.h"

#include <chrono>
#include <string>
#include <vector>

#include <unistd.h>

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
    // UTA requires uppercase H for hour intervals.
    EXPECT_EQ(m4.interval, "4H");
}

TEST(BitgetTransportSubscribe, NormalizeKlineIntervalCase)
{
    EXPECT_EQ(bitget::normalize_kline_interval("1m"), "1m");
    EXPECT_EQ(bitget::normalize_kline_interval("15m"), "15m");
    EXPECT_EQ(bitget::normalize_kline_interval("1h"), "1H");
    EXPECT_EQ(bitget::normalize_kline_interval("4h"), "4H");
    EXPECT_EQ(bitget::normalize_kline_interval("4H"), "4H");
    EXPECT_EQ(bitget::normalize_kline_interval("1d"), "1D");
    EXPECT_EQ(bitget::normalize_kline_interval("1D"), "1D");
    EXPECT_EQ(bitget::normalize_kline_interval("1w"), "1W");

    auto h = bitget::map_stream_to_topic("kline1h");
    EXPECT_EQ(h.topic, "kline");
    EXPECT_EQ(h.interval, "1H");

    auto d = bitget::map_stream_to_topic("kline1d");
    EXPECT_EQ(d.topic, "kline");
    EXPECT_EQ(d.interval, "1D");
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

TEST(BitgetTransportSubscribe, HeartbeatRequiresPongBeforeDeadline)
{
    using action = bitget::TextHeartbeat::action;
    bitget::TextHeartbeat heartbeat;
    const auto start = std::chrono::steady_clock::time_point{
        std::chrono::seconds{100}};
    heartbeat.reset(start);
    EXPECT_EQ(heartbeat.poll(start + std::chrono::seconds{29}), action::idle);
    EXPECT_EQ(heartbeat.poll(start + std::chrono::seconds{30}),
              action::send_ping);
    heartbeat.ping_sent(start + std::chrono::seconds{30});
    EXPECT_EQ(heartbeat.poll(start + std::chrono::seconds{39}), action::idle);
    EXPECT_EQ(heartbeat.poll(start + std::chrono::seconds{40}), action::failed);

    heartbeat.pong_received();
    EXPECT_EQ(heartbeat.poll(start + std::chrono::seconds{40}), action::idle);
    EXPECT_EQ(heartbeat.poll(start + std::chrono::seconds{40}, true),
              action::send_ping);
}

TEST(BitgetTransportSubscribe, PollFdReadableTimeoutAndReady)
{
    int fds[2] = {-1, -1};
    ASSERT_EQ(::pipe(fds), 0);

    // Empty pipe → short poll times out (wake path for app ping).
    auto t0 = std::chrono::steady_clock::now();
    auto r = bitget::poll_fd_readable(fds[0], std::chrono::milliseconds(50));
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_EQ(r, bitget::poll_wait_result::timeout);
    EXPECT_GE(elapsed, std::chrono::milliseconds(30));

    // Write one byte → readable without waiting full timeout.
    ASSERT_EQ(::write(fds[1], "x", 1), 1);
    r = bitget::poll_fd_readable(fds[0], std::chrono::milliseconds(500));
    EXPECT_EQ(r, bitget::poll_wait_result::ready);

    EXPECT_EQ(bitget::poll_fd_readable(-1, std::chrono::milliseconds(1)),
              bitget::poll_wait_result::error);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(BitgetTransportSubscribe, SslPendingNullIsFalse)
{
    EXPECT_FALSE(bitget::ssl_has_pending_app_data(nullptr));
}

#endif // HAS_BITGET
