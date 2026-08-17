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

template<class Fn>
void expect_runtime_error_contains(Fn&& fn, std::string_view needle)
{
    try
    {
        fn();
        FAIL() << "expected std::runtime_error containing " << needle;
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_NE(std::string_view{error.what()}.find(needle),
                  std::string_view::npos)
            << error.what();
    }
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
    EXPECT_NE(io->posts[0].second.find("\"reduceOnly\":\"yes\""), std::string::npos);
    EXPECT_NE(io->posts[0].second.find("\"clientOid\":\"tt-fb-7\""), std::string::npos);
    EXPECT_NE(io->posts[0].second.find("\"stopLoss\":\"90000\""), std::string::npos);
    EXPECT_NE(io->posts[0].second.find("\"takeProfit\":\"110000\""), std::string::npos);
}

TEST(BitgetFuturesBracketAdapter, PlaceMalformedOrAmbiguousSuccessThrows)
{
    auto io = std::make_shared<fake_io>();
    BitgetFuturesBracketAdapter a(
        [io](std::string_view e, std::string_view b) { return io->post(e, b); },
        nullptr);

    const std::string payloads[] = {
        R"({"code":"00000"})",
        R"({"code":"00000","data":{"orderId":"99","orderId":"100","clientOid":"tt-fb-7"}})",
        R"({"code":"00000","data":{"orderId":"99","clientOid":"tt-fb-8"}})",
        R"({"code":"00000","data":{"orderId":"junk","clientOid":"tt-fb-7"}})",
        R"({"code":"00000","data":{"nested":{"orderId":"99","clientOid":"tt-fb-7"}}})",
    };
    for (const auto& payload : payloads)
    {
        io->post_resp = {200, payload};
        EXPECT_THROW(a.place(7, full_intent(), 100000.0), std::runtime_error)
            << payload;
    }
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
    io->post_resp = {200, R"({"code":"00000","msg":"success","data":null})"};
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

TEST(BitgetFuturesBracketAdapter, CancelMalformedSuccessThrows)
{
    auto io = std::make_shared<fake_io>();
    io->post_resp = {
        200, R"({"code":"00000","nested":{"msg":"success","data":null}})"};
    BitgetFuturesBracketAdapter a(
        [io](std::string_view e, std::string_view b) { return io->post(e, b); },
        nullptr);
    truetest::exits::bracket_handles h;
    h.oco_list_id = "99";
    EXPECT_THROW(a.cancel(7, h), std::runtime_error);
}

TEST(BitgetFuturesBracketAdapter, ListOpenRefusesBrandedForeignSymbol)
{
    auto io = std::make_shared<fake_io>();
    io->get_resp = {200,
        R"({"code":"00000","data":[{
          "orderId":"55","clientOid":"tt-fb-42","symbol":"BTCUSDT",
          "category":"USDT-FUTURES","status":"pending",
          "side":"sell","stopLoss":"90000","takeProfit":"110000"
        },{
          "orderId":"77","clientOid":"tt-fb-77","symbol":"ETHUSDT",
          "category":"USDT-FUTURES","status":"pending",
          "side":"sell","stopLoss":"3000","takeProfit":"4000"
        }]})"};
    BitgetFuturesBracketAdapter a(
        nullptr,
        [io](std::string_view e, std::string_view q) { return io->get(e, q); },
        "USDT-FUTURES", "BTCUSDT");

    expect_runtime_error_contains(
        [&] { (void)a.list_open(); }, "unexpected symbol");
}

TEST(BitgetFuturesBracketAdapter, ListOpenIgnoresUnbrandedForeignSymbol)
{
    auto io = std::make_shared<fake_io>();
    io->get_resp = {200,
        R"({"code":"00000","data":[{
          "orderId":"55","clientOid":"tt-fb-42","symbol":"BTCUSDT",
          "category":"USDT-FUTURES","status":"pending",
          "side":"sell","stopLoss":"90000","takeProfit":"110000"
        },{
          "orderId":"77","clientOid":"other-tool","symbol":"ETHUSDT",
          "category":"USDT-FUTURES","status":"pending",
          "side":"sell","stopLoss":"3000","takeProfit":"4000"
        }]})"};
    BitgetFuturesBracketAdapter a(
        nullptr,
        [io](std::string_view e, std::string_view q) { return io->get(e, q); },
        "USDT-FUTURES", "BTCUSDT");

    const auto recovered = a.list_open();
    ASSERT_EQ(recovered.size(), 1u);
    EXPECT_EQ(recovered.front().opener_order_id, 42u);
    EXPECT_EQ(recovered.front().symbol, "BTCUSDT");
}

TEST(BitgetFuturesBracketAdapter, ListOpenRequiresMatchingCategoryAndOpenStatus)
{
    const std::vector<std::string> rows = {
        R"({"orderId":"55","clientOid":"tt-fb-42","symbol":"BTCUSDT","category":"SPOT","status":"pending","side":"sell","stopLoss":"90","takeProfit":"110"})",
        R"({"orderId":"55","clientOid":"tt-fb-42","symbol":"BTCUSDT","category":"USDT-FUTURES","status":"cancelled","side":"sell","stopLoss":"90","takeProfit":"110"})",
        R"({"orderId":"55","clientOid":"tt-fb-42","symbol":"BTCUSDT","category":"USDT-FUTURES","side":"sell","stopLoss":"90","takeProfit":"110"})",
        R"({"orderId":"55","clientOid":"tt-fb-42","symbol":"BTCUSDT","category":"USDT-FUTURES","status":"pending","status":"success","side":"sell","stopLoss":"90","takeProfit":"110"})",
    };
    for (const auto& row : rows)
    {
        auto io = std::make_shared<fake_io>();
        io->get_resp = {200, "{\"code\":\"00000\",\"data\":[" + row + "]}"};
        BitgetFuturesBracketAdapter a(
            nullptr, [io](std::string_view e, std::string_view q) {
                return io->get(e, q);
            });
        EXPECT_THROW(a.list_open(), std::runtime_error) << row;
    }
}

TEST(BitgetFuturesBracketAdapter, ListOpenWithoutGetCallableRefusesRecovery)
{
    BitgetFuturesBracketAdapter a(nullptr, nullptr);
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BitgetFuturesBracketAdapter, ListOpenHttpFailureRefusesRecovery)
{
    auto io = std::make_shared<fake_io>();
    io->get_resp = {503, R"({"code":"50000","msg":"unavailable"})"};
    BitgetFuturesBracketAdapter a(
        nullptr,
        [io](std::string_view e, std::string_view q) { return io->get(e, q); });
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BitgetFuturesBracketAdapter, ListOpenMalformedSuccessRefusesRecovery)
{
    auto io = std::make_shared<fake_io>();
    io->get_resp = {200, R"({"code":"00000","unexpected":[]})"};
    BitgetFuturesBracketAdapter a(
        nullptr,
        [io](std::string_view e, std::string_view q) { return io->get(e, q); });
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BitgetFuturesBracketAdapter, ListOpenMissingIdentityRefusesRecovery)
{
    auto io = std::make_shared<fake_io>();
    io->get_resp = {200, R"({"code":"00000","data":[{}]})"};
    BitgetFuturesBracketAdapter a(
        nullptr,
        [io](std::string_view e, std::string_view q) { return io->get(e, q); });
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BitgetFuturesBracketAdapter, ListOpenDuplicateRowOrOpenerRefusesRecovery)
{
    auto io = std::make_shared<fake_io>();
    io->get_resp = {200, R"({"code":"00000","data":[
      {"orderId":"1","clientOid":"tt-fb-7","clientOid":"tt-fb-8","symbol":"BTCUSDT","category":"USDT-FUTURES","status":"pending","side":"sell","stopLoss":"90","takeProfit":"110"}
    ]})"};
    BitgetFuturesBracketAdapter a(
        nullptr, [io](std::string_view e, std::string_view q) {
            return io->get(e, q);
        });
    EXPECT_THROW(a.list_open(), std::runtime_error);

    io->get_resp = {200, R"({"code":"00000","data":[
      {"orderId":"1","clientOid":"tt-fb-7","symbol":"BTCUSDT","category":"USDT-FUTURES","status":"pending","side":"sell","stopLoss":"90","takeProfit":"110"},
      {"orderId":"2","clientOid":"tt-fb-7","symbol":"BTCUSDT","category":"USDT-FUTURES","status":"pending","side":"sell","stopLoss":"91","takeProfit":"111"}
    ]})"};
    expect_runtime_error_contains(
        [&] { (void)a.list_open(); }, "duplicate TrueTest opener");
}

TEST(BitgetFuturesBracketAdapter, ListOpenInvalidSemanticFieldsRefusesRecovery)
{
    auto io = std::make_shared<fake_io>();
    io->get_resp = {200, R"({"code":"00000","data":[{
      "orderId":"1","clientOid":"tt-fb-7","symbol":"BTCUSDT",
      "category":"USDT-FUTURES","status":"pending",
      "side":"unknown","stopLoss":"junk","takeProfit":"110"
    }]})"};
    BitgetFuturesBracketAdapter a(
        nullptr, [io](std::string_view e, std::string_view q) {
            return io->get(e, q);
        });
    expect_runtime_error_contains(
        [&] { (void)a.list_open(); }, "protection legs incomplete");
}

TEST(BitgetFuturesBracketAdapter, ListOpenTrailingIdentityRefusesRecovery)
{
    auto io = std::make_shared<fake_io>();
    io->get_resp = {200, R"({"code":"00000","data":[{
      "orderId":"1","clientOid":"tt-fb-7evil","symbol":"BTCUSDT",
      "category":"USDT-FUTURES","status":"pending",
      "side":"sell","stopLoss":"90","takeProfit":"110"
    }]})"};
    BitgetFuturesBracketAdapter a(
        nullptr, [io](std::string_view e, std::string_view q) {
            return io->get(e, q);
        });
    expect_runtime_error_contains(
        [&] { (void)a.list_open(); }, "invalid TrueTest clientOid");
}

TEST(BitgetFuturesBracketAdapter, ListOpenDuplicateVenueOrderIdRefusesRecovery)
{
    auto io = std::make_shared<fake_io>();
    io->get_resp = {200, R"({"code":"00000","data":[
      {"orderId":"1","clientOid":"tt-fb-7","symbol":"BTCUSDT","category":"USDT-FUTURES","status":"pending","side":"sell","stopLoss":"90","takeProfit":"110"},
      {"orderId":"1","clientOid":"tt-fb-8","symbol":"BTCUSDT","category":"USDT-FUTURES","status":"pending","side":"sell","stopLoss":"91","takeProfit":"111"}
    ]})"};
    BitgetFuturesBracketAdapter a(
        nullptr, [io](std::string_view e, std::string_view q) {
            return io->get(e, q);
        });
    expect_runtime_error_contains(
        [&] { (void)a.list_open(); }, "duplicate venue orderId");
}

#endif // HAS_BITGET
