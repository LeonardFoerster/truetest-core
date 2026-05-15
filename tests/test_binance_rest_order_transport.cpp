#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_rest_order_transport.h"

#include <string>
#include <string_view>

namespace {

struct fake_caller
{
    int status = 200;
    std::string body;
    std::string last_endpoint;
    std::string last_params;
    int calls = 0;

    BinanceRestOrderTransport::response operator()(std::string_view ep,
                                                   std::string_view params)
    {
        last_endpoint = std::string(ep);
        last_params   = std::string(params);
        ++calls;
        return {status, body};
    }
};

}

TEST(BinanceRestOrderTransport, SubmitSuccessExtractsExchangeOrderId)
{
    auto post = std::make_shared<fake_caller>();
    post->status = 200;
    post->body   = R"({"orderId":999123,"status":"NEW"})";
    auto del     = std::make_shared<fake_caller>();

    BinanceRestOrderTransport tx(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); });

    auto r = tx.submit("/api/v3/order", "symbol=BTCUSDT&side=BUY");

    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.exchange_order_id, "999123");
    EXPECT_EQ(r.raw_response, post->body);
    EXPECT_EQ(post->last_endpoint, "/api/v3/order");
    EXPECT_EQ(post->last_params, "symbol=BTCUSDT&side=BUY");
    EXPECT_EQ(del->calls, 0);
}

TEST(BinanceRestOrderTransport, SubmitSuccessHandlesStringOrderId)
{
    auto post = std::make_shared<fake_caller>();
    post->status = 201;
    post->body   = R"({"orderId":"abc-123"})";

    BinanceRestOrderTransport tx(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        {});

    auto r = tx.submit("/api/v3/order", "");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.exchange_order_id, "abc-123");
}

TEST(BinanceRestOrderTransport, SubmitNon2xxReturnsError)
{
    auto post = std::make_shared<fake_caller>();
    post->status = 400;
    post->body   = R"({"code":-1021,"msg":"Timestamp for this request was 1000ms ahead."})";

    BinanceRestOrderTransport tx(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        {});

    auto r = tx.submit("/api/v3/order", "bad=1");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.exchange_order_id.empty());
    EXPECT_NE(r.error.find("HTTP 400"), std::string::npos);
    EXPECT_NE(r.error.find("Timestamp"), std::string::npos);
}

TEST(BinanceRestOrderTransport, CancelRoutesToDelCallable)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    del->status = 200;
    del->body   = R"({"status":"CANCELED"})";

    BinanceRestOrderTransport tx(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); });

    auto r = tx.cancel("/api/v3/order", "symbol=BTCUSDT&orderId=999");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(del->last_params, "symbol=BTCUSDT&orderId=999");
    EXPECT_EQ(post->calls, 0);
}

TEST(BinanceRestOrderTransport, SubmitNullCallableReturnsError)
{
    BinanceRestOrderTransport tx({}, {});
    auto r = tx.submit("/api/v3/order", "x=1");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST(BinanceRestOrderTransport, CancelNullCallableReturnsError)
{
    BinanceRestOrderTransport tx({}, {});
    auto r = tx.cancel("/api/v3/order", "x=1");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST(BinanceRestOrderTransport, OpenCloseAreNoOps)
{
    BinanceRestOrderTransport tx({}, {});
    EXPECT_TRUE(tx.open());
    tx.close();
}

#endif // HAS_BINANCE
