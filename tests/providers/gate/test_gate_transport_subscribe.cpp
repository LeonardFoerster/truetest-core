#include <gtest/gtest.h>

#ifdef HAS_GATE

#include "providers/gate/gate_transport.h"

#include <chrono>
#include <string>
#include <vector>

#include <unistd.h>

// --- map_stream_to_channel ---

TEST(GateTransportSubscribe, MapTrades)
{
    auto m = gate::map_stream_to_channel("trades");
    EXPECT_EQ(m.channel, "futures.trades");
    EXPECT_TRUE(m.interval.empty());

    auto m2 = gate::map_stream_to_channel("trade");
    EXPECT_EQ(m2.channel, "futures.trades");
}

TEST(GateTransportSubscribe, MapDepthSpec)
{
    auto m = gate::map_stream_to_channel("100ms:100");
    EXPECT_EQ(m.channel, "futures.order_book_update");
    EXPECT_EQ(m.depth_freq, "100ms");
    EXPECT_EQ(m.depth_level, "100");
}

TEST(GateTransportSubscribe, MapOrderBookAlias)
{
    auto m = gate::map_stream_to_channel("order_book_update");
    EXPECT_EQ(m.channel, "futures.order_book_update");
    EXPECT_EQ(m.depth_freq, "100ms");
    EXPECT_EQ(m.depth_level, "100");
}

TEST(GateTransportSubscribe, MapKline)
{
    auto m = gate::map_stream_to_channel("kline1m");
    EXPECT_EQ(m.channel, "futures.candlesticks");
    EXPECT_EQ(m.interval, "1m");

    auto m2 = gate::map_stream_to_channel("candle5m");
    EXPECT_EQ(m2.channel, "futures.candlesticks");
    EXPECT_EQ(m2.interval, "5m");
}

TEST(GateTransportSubscribe, MapAlreadyMappedPassThrough)
{
    auto m = gate::map_stream_to_channel("futures.trades");
    EXPECT_EQ(m.channel, "futures.trades");
}

// --- subscribe JSON builders ---

TEST(GateTransportSubscribe, SubscribeJsonTradesExact)
{
    const std::string j =
        gate::build_subscribe_json("BTC_USDT", "trades", /*time_s=*/1710000000);
    EXPECT_EQ(
        j,
        R"({"time":1710000000,"channel":"futures.trades","event":"subscribe","payload":["BTC_USDT"]})");
}

TEST(GateTransportSubscribe, SubscribeJsonOrderBookExact)
{
    const std::string j = gate::build_subscribe_json(
        "BTC_USDT", "100ms:100", /*time_s=*/1710000000);
    EXPECT_EQ(
        j,
        R"({"time":1710000000,"channel":"futures.order_book_update","event":"subscribe","payload":["BTC_USDT","100ms","100"]})");
}

TEST(GateTransportSubscribe, SubscribeJsonCandlesticksExact)
{
    const std::string j = gate::build_subscribe_json(
        "ETH_USDT", "kline1m", /*time_s=*/1710000000);
    EXPECT_EQ(
        j,
        R"({"time":1710000000,"channel":"futures.candlesticks","event":"subscribe","payload":["1m","ETH_USDT"]})");
}

TEST(GateTransportSubscribe, MultiSubscribeJsons)
{
    const std::vector<std::string> streams = {"trades", "100ms:100"};
    auto js = gate::build_subscribe_jsons_for_streams(
        "BTC_USDT", streams, /*time_s=*/100);
    ASSERT_EQ(js.size(), 2u);
    EXPECT_NE(js[0].find("futures.trades"), std::string::npos);
    EXPECT_NE(js[1].find("futures.order_book_update"), std::string::npos);
    EXPECT_NE(js[1].find("100ms"), std::string::npos);
}

TEST(GateTransportSubscribe, PingPongHelpers)
{
    auto ping = gate::build_ping_json(1710000000);
    EXPECT_EQ(ping,
              R"({"time":1710000000,"channel":"futures.ping"})");
    EXPECT_TRUE(gate::is_ping_frame(ping));
    EXPECT_FALSE(gate::is_pong_frame(ping));

    constexpr auto pong =
        R"({"time":1710000000,"channel":"futures.pong","event":"","result":null})";
    EXPECT_TRUE(gate::is_pong_frame(pong));
    EXPECT_FALSE(gate::is_ping_frame(pong));
}

TEST(GateTransportSubscribe, PollFdReadableTimeoutAndReady)
{
    int fds[2] = {-1, -1};
    ASSERT_EQ(::pipe(fds), 0);

    auto t0 = std::chrono::steady_clock::now();
    auto r = gate::poll_fd_readable(fds[0], std::chrono::milliseconds(50));
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_EQ(r, gate::poll_wait_result::timeout);
    EXPECT_GE(elapsed, std::chrono::milliseconds(30));

    ASSERT_EQ(::write(fds[1], "x", 1), 1);
    r = gate::poll_fd_readable(fds[0], std::chrono::milliseconds(500));
    EXPECT_EQ(r, gate::poll_wait_result::ready);

    EXPECT_EQ(gate::poll_fd_readable(-1, std::chrono::milliseconds(1)),
              gate::poll_wait_result::error);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(GateTransportSubscribe, SslPendingNullIsFalse)
{
    EXPECT_FALSE(gate::ssl_has_pending_app_data(nullptr));
}

#endif // HAS_GATE
