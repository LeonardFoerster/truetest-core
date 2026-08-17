// Pins the wire format and parsing of BinanceOcoBracketAdapter - the
// venue-bracket adapter for Binance spot. Uses the fake-callable pattern
// so no real REST call ever happens.

#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_oco_bracket_adapter.h"
#include "exits/exit_intent.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct fake_caller
{
    int status = 200;
    std::string body;
    std::string last_endpoint;
    std::string last_params;
    int calls = 0;

    BinanceOcoBracketAdapter::response operator()(std::string_view ep,
                                                  std::string_view p)
    {
        last_endpoint = std::string(ep);
        last_params   = std::string(p);
        ++calls;
        return {status, body};
    }
};

truetest::exits::exit_intent make_long_intent(std::uint64_t opener,
                                              double sl, double tp,
                                              double qty = 0.5)
{
    truetest::exits::exit_intent ei;
    ei.symbol           = "btcusdt";
    ei.close_side       = order_side::sell;
    ei.qty              = qty;
    ei.stop_loss        = sl;
    ei.take_profit      = tp;
    ei.opener_order_id  = opener;
    ei.strategy_name    = "s";
    return ei;
}

}

TEST(BinanceOcoBracketAdapter, AdvertisesOcoCapability)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); });

    auto c = a.capabilities();
    EXPECT_TRUE(c.oco);
    EXPECT_TRUE(c.stop_limit);
    EXPECT_FALSE(c.stop_market);
    EXPECT_FALSE(c.trailing_stop);
}

TEST(BinanceOcoBracketAdapter, PlaceBuildsOcoQueryAndParsesHandles)
{
    auto post = std::make_shared<fake_caller>();
    post->status = 200;
    post->body   = R"({
        "orderListId":42,
        "listClientOrderId":"tt-oco-7",
        "contingencyType":"OCO",
        "symbol":"BTCUSDT",
        "orders":[
            {"symbol":"BTCUSDT","orderId":111,"clientOrderId":"x"},
            {"symbol":"BTCUSDT","orderId":222,"clientOrderId":"y"}
        ],
        "orderReports":[
            {"symbol":"BTCUSDT","orderId":111,"type":"STOP_LOSS_LIMIT","status":"NEW"},
            {"symbol":"BTCUSDT","orderId":222,"type":"LIMIT_MAKER","status":"NEW"}
        ]
    })";
    auto del = std::make_shared<fake_caller>();

    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); });

    auto h = a.place(/*opener=*/7, make_long_intent(7, /*sl=*/95.0, /*tp=*/110.0), 100.0);

    // Endpoint + payload shape: must hit OCO endpoint, must encode opener
    // id in listClientOrderId so reconciler can rehydrate after restart.
    EXPECT_EQ(post->last_endpoint, "/api/v3/order/oco");
    EXPECT_NE(post->last_params.find("symbol=BTCUSDT"), std::string::npos);
    EXPECT_NE(post->last_params.find("side=SELL"),     std::string::npos);
    EXPECT_NE(post->last_params.find("price=110"),     std::string::npos);
    EXPECT_NE(post->last_params.find("stopPrice=95"),  std::string::npos);
    EXPECT_NE(post->last_params.find("stopLimitTimeInForce=GTC"), std::string::npos);
    EXPECT_NE(post->last_params.find("listClientOrderId=tt-oco-7"), std::string::npos);

    // Handles populated for both legs + group id.
    ASSERT_TRUE(h.oco_list_id);
    EXPECT_EQ(*h.oco_list_id, "42");
    ASSERT_TRUE(h.sl_exchange_id);
    EXPECT_EQ(*h.sl_exchange_id, "111");
    ASSERT_TRUE(h.tp_exchange_id);
    EXPECT_EQ(*h.tp_exchange_id, "222");
}

TEST(BinanceOcoBracketAdapter, PlaceWithMissingSlOrTpDeclines)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); });

    truetest::exits::exit_intent only_sl = make_long_intent(1, 95.0, 0.0);
    only_sl.take_profit.reset();
    auto h = a.place(1, only_sl, 100.0);
    EXPECT_TRUE(h.empty());
    EXPECT_EQ(post->calls, 0);  // no REST call made
}

TEST(BinanceOcoBracketAdapter, PlaceHttpFailureReturnsEmptyHandles)
{
    auto post = std::make_shared<fake_caller>();
    post->status = 400;
    post->body   = R"({"code":-2010,"msg":"Account has insufficient balance"})";
    auto del     = std::make_shared<fake_caller>();

    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); });

    auto h = a.place(7, make_long_intent(7, 95.0, 110.0), 100.0);
    EXPECT_TRUE(h.empty());
}

TEST(BinanceOcoBracketAdapter, PlaceMalformedOrAmbiguousSuccessThrows)
{
    auto post = std::make_shared<fake_caller>();
    auto del = std::make_shared<fake_caller>();
    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del](std::string_view ep, std::string_view p) { return (*del)(ep, p); });

    const std::string payloads[] = {
        R"({"orderListId":42})",
        R"({"orderListId":42,"orderListId":43,"listClientOrderId":"tt-oco-7","symbol":"BTCUSDT","orders":[],"orderReports":[]})",
        R"({"orderListId":42,"listClientOrderId":"tt-oco-7","symbol":"BTCUSDT","orders":[{"orderId":111,"clientOrderId":"x"},{"orderId":111,"clientOrderId":"y"}],"orderReports":[{"orderId":111,"type":"STOP_LOSS_LIMIT"},{"orderId":111,"type":"LIMIT_MAKER"}]})",
        R"({"orderListId":42,"listClientOrderId":"tt-oco-7","symbol":"BTCUSDT","orders":[{"orderId":111,"clientOrderId":"x"},{"orderId":222,"clientOrderId":"y"}],"orderReports":[{"orderId":111,"type":"STOP_LOSS_LIMIT"},{"orderId":222,"type":"UNKNOWN"}]})",
        R"({"nested":{"orderListId":42,"listClientOrderId":"tt-oco-7","symbol":"BTCUSDT","orders":[],"orderReports":[]}})",
    };
    for (const auto& payload : payloads)
    {
        post->body = payload;
        EXPECT_THROW(
            a.place(7, make_long_intent(7, 95.0, 110.0), 100.0),
            std::runtime_error) << payload;
    }
}

TEST(BinanceOcoBracketAdapter, CancelWithListIdHitsOrderListEndpoint)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    del->status = 200;
    del->body   = R"({"orderListId":42,"listStatusType":"ALL_DONE","listOrderStatus":"ALL_DONE"})";

    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); });

    truetest::exits::bracket_handles h;
    h.oco_list_id    = "42";
    h.sl_exchange_id = "111";
    h.tp_exchange_id = "222";

    a.cancel(7, h);
    EXPECT_EQ(del->calls, 1);
    EXPECT_EQ(del->last_endpoint, "/api/v3/orderList");
    EXPECT_NE(del->last_params.find("orderListId=42"), std::string::npos);
}

TEST(BinanceOcoBracketAdapter, CancelFallbackToPerLegIfOrderListFails)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    del->status = 400;
    del->body   = R"({"code":-2011,"msg":"Unknown order"})";

    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); });

    truetest::exits::bracket_handles h;
    h.oco_list_id    = "42";
    h.sl_exchange_id = "111";
    h.tp_exchange_id = "222";

    a.cancel(7, h);
    // 1 orderList attempt + 2 per-leg fallback DELETEs
    EXPECT_EQ(del->calls, 3);
}

TEST(BinanceOcoBracketAdapter, CancelMalformedSuccessThrows)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    del->status = 200;
    del->body =
        R"({"nested":{"orderListId":42,"listStatusType":"ALL_DONE"}})";
    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del](std::string_view ep, std::string_view p) { return (*del)(ep, p); });
    truetest::exits::bracket_handles h;
    h.oco_list_id = "42";
    EXPECT_THROW(a.cancel(7, h), std::runtime_error);
}

TEST(BinanceOcoBracketAdapter, GroupCancelRequiresExactConsistentIdentityAndState)
{
    const std::vector<std::string> unproven = {
        R"({"orderListId":42,"listStatusType":"ALL_DONE","listOrderStatus":"EXECUTING"})",
        R"({"orderListId":43,"listStatusType":"ALL_DONE","listOrderStatus":"ALL_DONE"})",
        R"({"orderListId":42,"orderListId":43,"listStatusType":"ALL_DONE","listOrderStatus":"ALL_DONE"})",
        R"({"orderListId":42,"listStatusType":"ALL_DONE","listStatusType":"EXEC_STARTED","listOrderStatus":"ALL_DONE"})",
    };
    for (const auto& body : unproven)
    {
        auto post = std::make_shared<fake_caller>();
        auto del = std::make_shared<fake_caller>();
        del->status = 200;
        del->body = body;
        BinanceOcoBracketAdapter a(
            [post](std::string_view ep, std::string_view p) {
                return (*post)(ep, p);
            },
            [del](std::string_view ep, std::string_view p) {
                return (*del)(ep, p);
            });
        truetest::exits::bracket_handles h;
        h.oco_list_id = "42";
        EXPECT_THROW(a.cancel(7, h), std::runtime_error) << body;
    }
}

TEST(BinanceOcoBracketAdapter, GroupCancelExceptionFallsBackToBothLegs)
{
    auto post = std::make_shared<fake_caller>();
    int calls = 0;
    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) {
            return (*post)(ep, p);
        },
        [&](std::string_view endpoint, std::string_view params)
            -> BinanceOcoBracketAdapter::response {
            ++calls;
            if (endpoint == "/api/v3/orderList")
                throw std::runtime_error("ambiguous grouped cancel");
            if (params.find("orderId=111") != std::string_view::npos)
                return {200, R"({"orderId":111,"status":"CANCELED"})"};
            return {200, R"({"orderId":222,"status":"CANCELED"})"};
        });
    truetest::exits::bracket_handles h;
    h.oco_list_id = "42";
    h.sl_exchange_id = "111";
    h.tp_exchange_id = "222";
    h.symbol = "BTCUSDT";

    EXPECT_NO_THROW(a.cancel(7, h));
    EXPECT_EQ(calls, 3);
}

TEST(BinanceOcoBracketAdapter, FirstLegExceptionStillAttemptsSecondLeg)
{
    auto post = std::make_shared<fake_caller>();
    int calls = 0;
    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) {
            return (*post)(ep, p);
        },
        [&](std::string_view, std::string_view params)
            -> BinanceOcoBracketAdapter::response {
            ++calls;
            if (params.find("orderId=111") != std::string_view::npos)
                throw std::runtime_error("ambiguous SL cancel");
            EXPECT_NE(params.find("orderId=222"), std::string_view::npos);
            return {200, R"({"orderId":222,"status":"CANCELED"})"};
        });
    truetest::exits::bracket_handles h;
    h.sl_exchange_id = "111";
    h.tp_exchange_id = "222";
    h.symbol = "BTCUSDT";

    EXPECT_THROW(a.cancel(7, h), std::runtime_error);
    EXPECT_EQ(calls, 2);
}

TEST(BinanceOcoBracketAdapter, CancelEmptyHandlesIsNoop)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); });

    a.cancel(7, {});
    EXPECT_EQ(del->calls, 0);
}

TEST(BinanceOcoBracketAdapter, ListOpenRecoversTrueTestBracketsOnly)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();

    // Two GETs in sequence: openOrderList then openOrders. Use a
    // single fake driven by which endpoint was last requested.
    auto get = std::make_shared<fake_caller>();
    get->status = 200;

    int call = 0;
    auto get_dispatch = [get, &call](std::string_view ep, std::string_view p)
        -> BinanceOcoBracketAdapter::response
    {
        get->last_endpoint = std::string(ep);
        get->last_params   = std::string(p);
        ++get->calls;
        ++call;
        if (call == 1)
        {
            // openOrderList - one tt-oco list and one foreign list to
            // confirm the adapter only picks up its own.
            return {200, R"([
                {"orderListId":42,"listClientOrderId":"tt-oco-7",
                 "symbol":"BTCUSDT","contingencyType":"OCO",
                 "orders":[
                    {"symbol":"BTCUSDT","orderId":111,"clientOrderId":"x"},
                    {"symbol":"BTCUSDT","orderId":222,"clientOrderId":"y"}]},
                {"orderListId":99,"listClientOrderId":"someone-else-1",
                 "symbol":"BTCUSDT","contingencyType":"OCO",
                 "orders":[
                    {"symbol":"BTCUSDT","orderId":333,"clientOrderId":"a"},
                    {"symbol":"BTCUSDT","orderId":444,"clientOrderId":"b"}]}
            ])"};
        }
        // openOrders
        return {200, R"([
            {"symbol":"BTCUSDT","orderId":111,"type":"STOP_LOSS_LIMIT",
             "side":"SELL","origQty":"0.5","price":"94.05","stopPrice":"95.0"},
            {"symbol":"BTCUSDT","orderId":222,"type":"LIMIT_MAKER",
             "side":"SELL","origQty":"0.5","price":"110.0","stopPrice":"0"},
            {"symbol":"BTCUSDT","orderId":333,"type":"STOP_LOSS_LIMIT",
             "side":"BUY","origQty":"1.0","price":"105.0","stopPrice":"104.0"},
            {"symbol":"BTCUSDT","orderId":444,"type":"LIMIT_MAKER",
             "side":"BUY","origQty":"1.0","price":"95.0","stopPrice":"0"}
        ])"};
    };

    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); },
        get_dispatch);

    auto recovered = a.list_open();
    ASSERT_EQ(recovered.size(), 1u);

    const auto& rb = recovered[0];
    EXPECT_EQ(rb.opener_order_id, 7u);
    EXPECT_EQ(rb.symbol, "BTCUSDT");
    EXPECT_EQ(rb.close_side, order_side::sell);
    EXPECT_DOUBLE_EQ(rb.qty, 0.5);
    ASSERT_TRUE(rb.stop_loss);
    EXPECT_DOUBLE_EQ(*rb.stop_loss, 95.0);
    ASSERT_TRUE(rb.take_profit);
    EXPECT_DOUBLE_EQ(*rb.take_profit, 110.0);
    ASSERT_TRUE(rb.handles.sl_exchange_id);
    EXPECT_EQ(*rb.handles.sl_exchange_id, "111");
    ASSERT_TRUE(rb.handles.tp_exchange_id);
    EXPECT_EQ(*rb.handles.tp_exchange_id, "222");
    ASSERT_TRUE(rb.handles.oco_list_id);
    EXPECT_EQ(*rb.handles.oco_list_id, "42");
}

TEST(BinanceOcoBracketAdapter, ListOpenWithoutGetCallableRefusesRecovery)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); });

    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BinanceOcoBracketAdapter, ListOpenHttpFailureRefusesRecovery)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    auto get  = std::make_shared<fake_caller>();
    get->status = 503;
    get->body = R"({"msg":"unavailable"})";
    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); },
        [get ](std::string_view ep, std::string_view p) { return (*get )(ep, p); });
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BinanceOcoBracketAdapter, ListOpenMalformedSuccessRefusesRecovery)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    auto get  = std::make_shared<fake_caller>();
    get->status = 200;
    get->body = R"({"unexpected":[]})";
    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); },
        [get ](std::string_view ep, std::string_view p) { return (*get )(ep, p); });
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BinanceOcoBracketAdapter, ListOpenMissingIdentityRefusesRecovery)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    auto get  = std::make_shared<fake_caller>();
    get->status = 200;
    get->body = "[{}]";
    BinanceOcoBracketAdapter a(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); },
        [get ](std::string_view ep, std::string_view p) { return (*get )(ep, p); });
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

TEST(BinanceOcoBracketAdapter, ListOpenRejectsAmbiguousNestedLegAndDuplicateOpener)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    int call = 0;
    auto ambiguous_leg = [&call](std::string_view, std::string_view) {
        if (++call == 1)
            return BinanceOcoBracketAdapter::response{200, R"([{
              "orderListId":42,"listClientOrderId":"tt-oco-7","symbol":"BTCUSDT",
              "orders":[{"orderId":1,"clientOrderId":"a"},{"orderId":2,"clientOrderId":"b"},{"orderId":3,"orderId":4,"clientOrderId":"c"}]
            }])"};
        return BinanceOcoBracketAdapter::response{200, R"([
          {"orderId":1,"type":"STOP_LOSS_LIMIT","side":"SELL","origQty":"1","price":"89","stopPrice":"90"},
          {"orderId":2,"type":"LIMIT_MAKER","side":"SELL","origQty":"1","price":"110","stopPrice":"0"}
        ])"};
    };
    BinanceOcoBracketAdapter a(
        [post](std::string_view e, std::string_view p) { return (*post)(e, p); },
        [del](std::string_view e, std::string_view p) { return (*del)(e, p); },
        ambiguous_leg);
    EXPECT_THROW(a.list_open(), std::runtime_error);

    call = 0;
    auto duplicate_opener = [&call](std::string_view, std::string_view) {
        if (++call == 1)
            return BinanceOcoBracketAdapter::response{200, R"([
              {"orderListId":42,"listClientOrderId":"tt-oco-7","symbol":"BTCUSDT","orders":[{"orderId":1,"clientOrderId":"a"},{"orderId":2,"clientOrderId":"b"}]},
              {"orderListId":43,"listClientOrderId":"tt-oco-7","symbol":"BTCUSDT","orders":[{"orderId":3,"clientOrderId":"c"},{"orderId":4,"clientOrderId":"d"}]}
            ])"};
        return BinanceOcoBracketAdapter::response{200, R"([
          {"orderId":1,"type":"STOP_LOSS_LIMIT","side":"SELL","origQty":"1","price":"89","stopPrice":"90"},
          {"orderId":2,"type":"LIMIT_MAKER","side":"SELL","origQty":"1","price":"110","stopPrice":"0"},
          {"orderId":3,"type":"STOP_LOSS_LIMIT","side":"SELL","origQty":"1","price":"88","stopPrice":"89"},
          {"orderId":4,"type":"LIMIT_MAKER","side":"SELL","origQty":"1","price":"111","stopPrice":"0"}
        ])"};
    };
    BinanceOcoBracketAdapter b(
        [post](std::string_view e, std::string_view p) { return (*post)(e, p); },
        [del](std::string_view e, std::string_view p) { return (*del)(e, p); },
        duplicate_opener);
    EXPECT_THROW(b.list_open(), std::runtime_error);
}

TEST(BinanceOcoBracketAdapter, ListOpenRejectsTrailingIdentityAndBadLegSemantics)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    int call = 0;
    auto get = [&call](std::string_view, std::string_view) {
        if (++call == 1)
            return BinanceOcoBracketAdapter::response{200, R"([{
              "orderListId":42,"listClientOrderId":"tt-oco-7evil","symbol":"BTCUSDT",
              "orders":[{"orderId":1,"clientOrderId":"a"},{"orderId":2,"clientOrderId":"b"}]
            }])"};
        return BinanceOcoBracketAdapter::response{200, "[]"};
    };
    BinanceOcoBracketAdapter a(
        [post](std::string_view e, std::string_view p) { return (*post)(e, p); },
        [del](std::string_view e, std::string_view p) { return (*del)(e, p); },
        get);
    EXPECT_THROW(a.list_open(), std::runtime_error);

    call = 0;
    auto bad_semantics = [&call](std::string_view, std::string_view) {
        if (++call == 1)
            return BinanceOcoBracketAdapter::response{200, R"([{
              "orderListId":42,"listClientOrderId":"tt-oco-7","symbol":"BTCUSDT",
              "orders":[{"orderId":1,"clientOrderId":"a"},{"orderId":2,"clientOrderId":"b"}]
            }])"};
        return BinanceOcoBracketAdapter::response{200, R"([
          {"orderId":1,"type":"UNKNOWN","side":"UNKNOWN","origQty":"junk","price":"junk","stopPrice":"junk"},
          {"orderId":2,"type":"LIMIT_MAKER","side":"SELL","origQty":"1","price":"110","stopPrice":"0"}
        ])"};
    };
    BinanceOcoBracketAdapter b(
        [post](std::string_view e, std::string_view p) { return (*post)(e, p); },
        [del](std::string_view e, std::string_view p) { return (*del)(e, p); },
        bad_semantics);
    EXPECT_THROW(b.list_open(), std::runtime_error);
}

TEST(BinanceOcoBracketAdapter, ListOpenDuplicateVenueOrderIdRefusesRecovery)
{
    auto post = std::make_shared<fake_caller>();
    auto del = std::make_shared<fake_caller>();
    int call = 0;
    auto get = [&call](std::string_view, std::string_view) {
        if (++call == 1)
            return BinanceOcoBracketAdapter::response{200, R"([
              {"orderListId":42,"listClientOrderId":"tt-oco-7","symbol":"BTCUSDT","orders":[{"orderId":1,"clientOrderId":"a"},{"orderId":2,"clientOrderId":"b"}]},
              {"orderListId":43,"listClientOrderId":"tt-oco-8","symbol":"BTCUSDT","orders":[{"orderId":1,"clientOrderId":"c"},{"orderId":4,"clientOrderId":"d"}]}
            ])"};
        return BinanceOcoBracketAdapter::response{200, R"([
          {"orderId":1,"type":"STOP_LOSS_LIMIT","side":"SELL","origQty":"1","price":"89","stopPrice":"90"},
          {"orderId":2,"type":"LIMIT_MAKER","side":"SELL","origQty":"1","price":"110","stopPrice":"0"},
          {"orderId":4,"type":"LIMIT_MAKER","side":"SELL","origQty":"1","price":"111","stopPrice":"0"}
        ])"};
    };
    BinanceOcoBracketAdapter a(
        [post](std::string_view e, std::string_view p) { return (*post)(e, p); },
        [del](std::string_view e, std::string_view p) { return (*del)(e, p); },
        get);
    EXPECT_THROW(a.list_open(), std::runtime_error);
}

#endif // HAS_BINANCE
