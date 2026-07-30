#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_futures_bracket_adapter.h"

#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct SilenceStderr
{
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceStderr() : orig(std::cerr.rdbuf(sink.rdbuf())) {}
    ~SilenceStderr() { std::cerr.rdbuf(orig); }
};

using response = BybitFuturesBracketAdapter::response;

struct fake_io
{
    std::vector<std::pair<std::string, std::string>> posts;
    std::vector<std::pair<std::string, std::string>> gets;
    std::vector<response> post_responses;
    response default_post{
        200, R"({"retCode":0,"retMsg":"OK","result":{"orderId":"99","orderLinkId":"tt-fb-sl-7"}})"};
    response get_resp{
        200, R"({"retCode":0,"result":{"list":[]}})"};
    std::size_t post_i = 0;

    response post(std::string_view ep, std::string_view body)
    {
        posts.emplace_back(std::string(ep), std::string(body));
        if (post_i < post_responses.size())
            return post_responses[post_i++];
        // Distinct ids for sequential place legs.
        if (posts.size() == 1)
            return {200, R"({"retCode":0,"result":{"orderId":"11"}})"};
        if (posts.size() == 2)
            return {200, R"({"retCode":0,"result":{"orderId":"22"}})"};
        return default_post;
    }
    response get(std::string_view ep, std::string_view q)
    {
        gets.emplace_back(std::string(ep), std::string(q));
        return get_resp;
    }
};

truetest::exits::exit_intent full_intent()
{
    truetest::exits::exit_intent i;
    i.symbol = "btcusdt";
    i.close_side = order_side::sell;
    i.qty = 0.01;
    i.stop_loss = 90000.0;
    i.take_profit = 110000.0;
    i.qty_fraction = 1.0;
    return i;
}

} // namespace

TEST(BybitFuturesBracketAdapter, Capabilities)
{
    BybitFuturesBracketAdapter a(nullptr, nullptr);
    auto c = a.capabilities();
    EXPECT_TRUE(c.stop_market);
    EXPECT_FALSE(c.oco);
    EXPECT_FALSE(c.stop_limit);
    EXPECT_FALSE(c.trailing_stop);
}

TEST(BybitFuturesBracketAdapter, PlacePostsTwoConditionalOrders)
{
    auto io = std::make_shared<fake_io>();
    BybitFuturesBracketAdapter a(
        [io](std::string_view e, std::string_view b) { return io->post(e, b); },
        [io](std::string_view e, std::string_view q) { return io->get(e, q); });

    auto h = a.place(7, full_intent(), /*fill=*/100000.0);
    ASSERT_TRUE(h.sl_exchange_id.has_value());
    EXPECT_EQ(*h.sl_exchange_id, "11");
    ASSERT_TRUE(h.tp_exchange_id.has_value());
    EXPECT_EQ(*h.tp_exchange_id, "22");
    EXPECT_EQ(h.symbol, "BTCUSDT");
    EXPECT_FALSE(h.oco_list_id.has_value());

    ASSERT_EQ(io->posts.size(), 2u);
    EXPECT_EQ(io->posts[0].first, "/v5/order/create");
    EXPECT_EQ(io->posts[1].first, "/v5/order/create");

    // SL first: fall trigger (2) for long close.
    EXPECT_NE(io->posts[0].second.find("\"orderLinkId\":\"tt-fb-sl-7\""),
              std::string::npos);
    EXPECT_NE(io->posts[0].second.find("\"triggerPrice\":\"90000\""),
              std::string::npos);
    EXPECT_NE(io->posts[0].second.find("\"triggerDirection\":2"),
              std::string::npos);
    EXPECT_NE(io->posts[0].second.find("\"reduceOnly\":true"),
              std::string::npos);
    EXPECT_NE(io->posts[0].second.find("\"closeOnTrigger\":true"),
              std::string::npos);
    EXPECT_NE(io->posts[0].second.find("\"side\":\"Sell\""),
              std::string::npos);

    // TP: rise trigger (1).
    EXPECT_NE(io->posts[1].second.find("\"orderLinkId\":\"tt-fb-tp-7\""),
              std::string::npos);
    EXPECT_NE(io->posts[1].second.find("\"triggerPrice\":\"110000\""),
              std::string::npos);
    EXPECT_NE(io->posts[1].second.find("\"triggerDirection\":1"),
              std::string::npos);
}

TEST(BybitFuturesBracketAdapter, ShortCloseTriggerDirections)
{
    auto io = std::make_shared<fake_io>();
    BybitFuturesBracketAdapter a(
        [io](std::string_view e, std::string_view b) { return io->post(e, b); },
        nullptr);
    auto i = full_intent();
    i.close_side = order_side::buy; // short close
    auto h = a.place(3, i, 1.0);
    ASSERT_FALSE(h.empty());
    // SL rises (1), TP falls (2) for short.
    EXPECT_NE(io->posts[0].second.find("\"triggerDirection\":1"),
              std::string::npos);
    EXPECT_NE(io->posts[1].second.find("\"triggerDirection\":2"),
              std::string::npos);
    EXPECT_NE(io->posts[0].second.find("\"side\":\"Buy\""),
              std::string::npos);
}

TEST(BybitFuturesBracketAdapter, MissingLegDeclines)
{
    SilenceStderr quiet;
    auto io = std::make_shared<fake_io>();
    BybitFuturesBracketAdapter a(
        [io](std::string_view e, std::string_view b) { return io->post(e, b); },
        nullptr);
    truetest::exits::exit_intent i = full_intent();
    i.take_profit.reset();
    auto h = a.place(1, i, 1.0);
    EXPECT_TRUE(h.empty());
    EXPECT_TRUE(io->posts.empty());
}

TEST(BybitFuturesBracketAdapter, PartialFractionDeclines)
{
    SilenceStderr quiet;
    auto io = std::make_shared<fake_io>();
    BybitFuturesBracketAdapter a(
        [io](std::string_view e, std::string_view b) { return io->post(e, b); },
        nullptr);
    auto i = full_intent();
    i.qty_fraction = 0.5;
    auto h = a.place(1, i, 1.0);
    EXPECT_TRUE(h.empty());
    EXPECT_TRUE(io->posts.empty());
}

TEST(BybitFuturesBracketAdapter, ZeroQtyDeclines)
{
    SilenceStderr quiet;
    auto io = std::make_shared<fake_io>();
    BybitFuturesBracketAdapter a(
        [io](std::string_view e, std::string_view b) { return io->post(e, b); },
        nullptr);
    auto i = full_intent();
    i.qty = 0.0;
    auto h = a.place(1, i, 1.0);
    EXPECT_TRUE(h.empty());
    EXPECT_TRUE(io->posts.empty());
}

TEST(BybitFuturesBracketAdapter, TpFailLeavesSlHandle)
{
    SilenceStderr quiet;
    auto io = std::make_shared<fake_io>();
    io->post_responses = {
        {200, R"({"retCode":0,"result":{"orderId":"11"}})"},
        {200, R"({"retCode":10001,"retMsg":"fail"})"},
    };
    BybitFuturesBracketAdapter a(
        [io](std::string_view e, std::string_view b) { return io->post(e, b); },
        nullptr);
    auto h = a.place(7, full_intent(), 1.0);
    ASSERT_TRUE(h.sl_exchange_id.has_value());
    EXPECT_EQ(*h.sl_exchange_id, "11");
    EXPECT_FALSE(h.tp_exchange_id.has_value());
    EXPECT_EQ(h.symbol, "BTCUSDT");
}

TEST(BybitFuturesBracketAdapter, CancelPostsBothLegs)
{
    auto io = std::make_shared<fake_io>();
    BybitFuturesBracketAdapter a(
        [io](std::string_view e, std::string_view b) { return io->post(e, b); },
        nullptr);
    truetest::exits::bracket_handles h;
    h.sl_exchange_id = "11";
    h.tp_exchange_id = "22";
    h.symbol = "BTCUSDT";
    a.cancel(7, h);
    ASSERT_EQ(io->posts.size(), 2u);
    EXPECT_EQ(io->posts[0].first, "/v5/order/cancel");
    EXPECT_EQ(io->posts[1].first, "/v5/order/cancel");
    EXPECT_NE(io->posts[0].second.find("\"orderId\":\"11\""),
              std::string::npos);
    EXPECT_NE(io->posts[1].second.find("\"orderId\":\"22\""),
              std::string::npos);
    EXPECT_NE(io->posts[0].second.find("\"symbol\":\"BTCUSDT\""),
              std::string::npos);
}

TEST(BybitFuturesBracketAdapter, ListOpenRecoversTtFbPrefix)
{
    auto io = std::make_shared<fake_io>();
    io->get_resp = {200,
        R"({"retCode":0,"result":{"list":[
          {"orderId":"55","orderLinkId":"tt-fb-sl-42","symbol":"BTCUSDT",
           "side":"Sell","qty":"0.01","triggerPrice":"90000"},
          {"orderId":"56","orderLinkId":"tt-fb-tp-42","symbol":"BTCUSDT",
           "side":"Sell","qty":"0.01","triggerPrice":"110000"}
        ]}})"};
    BybitFuturesBracketAdapter a(
        nullptr,
        [io](std::string_view e, std::string_view q) { return io->get(e, q); });

    auto rec = a.list_open();
    ASSERT_EQ(rec.size(), 1u);
    EXPECT_EQ(rec[0].opener_order_id, 42u);
    EXPECT_EQ(rec[0].symbol, "BTCUSDT");
    ASSERT_TRUE(rec[0].stop_loss.has_value());
    EXPECT_DOUBLE_EQ(*rec[0].stop_loss, 90000.0);
    ASSERT_TRUE(rec[0].take_profit.has_value());
    EXPECT_DOUBLE_EQ(*rec[0].take_profit, 110000.0);
    ASSERT_TRUE(rec[0].handles.sl_exchange_id.has_value());
    EXPECT_EQ(*rec[0].handles.sl_exchange_id, "55");
    ASSERT_TRUE(rec[0].handles.tp_exchange_id.has_value());
    EXPECT_EQ(*rec[0].handles.tp_exchange_id, "56");
    EXPECT_EQ(io->gets[0].first, "/v5/order/realtime");
}

#endif // HAS_BYBIT
