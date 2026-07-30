#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_futures_bracket_adapter.h"

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

using response = BitgetFuturesBracketAdapter::response;

struct fake_io
{
    std::vector<std::pair<std::string, std::string>> posts;
    std::vector<std::pair<std::string, std::string>> gets;
    response post_resp{200, R"({"code":"00000","data":{"orderId":"99","clientOid":"tt-fb-7"}})"};
    response get_resp{200, R"({"code":"00000","data":[]})"};

    response post(std::string_view ep, std::string_view body)
    {
        posts.emplace_back(std::string(ep), std::string(body));
        return post_resp;
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
    i.stop_loss = 90000.0;
    i.take_profit = 110000.0;
    i.qty_fraction = 1.0;
    return i;
}

} // namespace

TEST(BitgetFuturesBracketAdapter, Capabilities)
{
    BitgetFuturesBracketAdapter a(nullptr, nullptr);
    auto c = a.capabilities();
    EXPECT_TRUE(c.stop_market);
    EXPECT_TRUE(c.oco);
    EXPECT_FALSE(c.stop_limit);
}

TEST(BitgetFuturesBracketAdapter, PlacePostsStrategyOrder)
{
    auto io = std::make_shared<fake_io>();
    BitgetFuturesBracketAdapter a(
        [io](std::string_view e, std::string_view b) { return io->post(e, b); },
        [io](std::string_view e, std::string_view q) { return io->get(e, q); });

    auto h = a.place(7, full_intent(), /*fill=*/100000.0);
    ASSERT_TRUE(h.sl_exchange_id.has_value());
    EXPECT_EQ(*h.sl_exchange_id, "99");
    EXPECT_EQ(*h.tp_exchange_id, "99");
    EXPECT_EQ(*h.oco_list_id, "99");
    EXPECT_EQ(h.symbol, "BTCUSDT");

    ASSERT_EQ(io->posts.size(), 1u);
    EXPECT_EQ(io->posts[0].first, "/api/v3/trade/place-strategy-order");
    EXPECT_NE(io->posts[0].second.find("\"type\":\"tpsl\""), std::string::npos);
    EXPECT_NE(io->posts[0].second.find("\"tpslMode\":\"full\""), std::string::npos);
    EXPECT_NE(io->posts[0].second.find("\"clientOid\":\"tt-fb-7\""), std::string::npos);
    EXPECT_NE(io->posts[0].second.find("\"stopLoss\":\"90000\""), std::string::npos);
    EXPECT_NE(io->posts[0].second.find("\"takeProfit\":\"110000\""), std::string::npos);
}

TEST(BitgetFuturesBracketAdapter, MissingLegDeclines)
{
    SilenceStderr quiet;
    auto io = std::make_shared<fake_io>();
    BitgetFuturesBracketAdapter a(
        [io](std::string_view e, std::string_view b) { return io->post(e, b); },
        nullptr);
    truetest::exits::exit_intent i = full_intent();
    i.take_profit.reset();
    auto h = a.place(1, i, 1.0);
    EXPECT_TRUE(h.empty());
    EXPECT_TRUE(io->posts.empty());
}

TEST(BitgetFuturesBracketAdapter, PartialFractionDeclines)
{
    SilenceStderr quiet;
    auto io = std::make_shared<fake_io>();
    BitgetFuturesBracketAdapter a(
        [io](std::string_view e, std::string_view b) { return io->post(e, b); },
        nullptr);
    auto i = full_intent();
    i.qty_fraction = 0.5;
    auto h = a.place(1, i, 1.0);
    EXPECT_TRUE(h.empty());
    EXPECT_TRUE(io->posts.empty());
}

TEST(BitgetFuturesBracketAdapter, CancelPostsOrderId)
{
    auto io = std::make_shared<fake_io>();
    BitgetFuturesBracketAdapter a(
        [io](std::string_view e, std::string_view b) { return io->post(e, b); },
        nullptr);
    truetest::exits::bracket_handles h;
    h.oco_list_id = "99";
    h.symbol = "BTCUSDT";
    a.cancel(7, h);
    ASSERT_EQ(io->posts.size(), 1u);
    EXPECT_EQ(io->posts[0].first, "/api/v3/trade/cancel-strategy-order");
    EXPECT_NE(io->posts[0].second.find("\"orderId\":\"99\""), std::string::npos);
}

TEST(BitgetFuturesBracketAdapter, ListOpenRecoversTtFbPrefix)
{
    auto io = std::make_shared<fake_io>();
    io->get_resp = {200,
        R"({"code":"00000","data":[{
          "orderId":"55","clientOid":"tt-fb-42","symbol":"BTCUSDT",
          "side":"sell","stopLoss":"90000","takeProfit":"110000"
        }]})"};
    BitgetFuturesBracketAdapter a(
        nullptr,
        [io](std::string_view e, std::string_view q) { return io->get(e, q); });

    auto rec = a.list_open();
    ASSERT_EQ(rec.size(), 1u);
    EXPECT_EQ(rec[0].opener_order_id, 42u);
    EXPECT_EQ(rec[0].symbol, "BTCUSDT");
    ASSERT_TRUE(rec[0].stop_loss.has_value());
    EXPECT_DOUBLE_EQ(*rec[0].stop_loss, 90000.0);
    ASSERT_TRUE(rec[0].handles.oco_list_id.has_value());
    EXPECT_EQ(*rec[0].handles.oco_list_id, "55");
}

#endif // HAS_BITGET
