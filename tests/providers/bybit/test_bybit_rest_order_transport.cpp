#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_rest_order_transport.h"

#include <memory>
#include <string>
#include <string_view>

namespace {

struct fake_post
{
    int status = 200;
    std::string body;
    std::string last_endpoint;
    std::string last_body;
    int calls = 0;

    BybitRestOrderTransport::response operator()(std::string_view ep,
                                                 std::string_view b)
    {
        last_endpoint = std::string(ep);
        last_body     = std::string(b);
        ++calls;
        return {status, body};
    }
};

} // namespace

TEST(BybitRestOrderTransport, SubmitSuccessExtractsOrderIdFromResult)
{
    auto post = std::make_shared<fake_post>();
    post->status = 200;
    post->body =
        R"({"retCode":0,"retMsg":"OK","result":{"orderId":"189918","orderLinkId":"tt-1"}})";

    BybitRestOrderTransport tx(
        [post](std::string_view ep, std::string_view b) { return (*post)(ep, b); });

    auto r = tx.submit("/v5/order/create",
                       R"({"category":"linear","symbol":"BTCUSDT"})");

    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.exchange_order_id, "189918");
    EXPECT_EQ(r.raw_response, post->body);
    EXPECT_EQ(post->last_endpoint, "/v5/order/create");
    EXPECT_EQ(post->calls, 1);
}

TEST(BybitRestOrderTransport, CancelUsesPostNotDelete)
{
    auto post = std::make_shared<fake_post>();
    post->status = 200;
    post->body =
        R"({"retCode":0,"retMsg":"OK","result":{"orderId":"42"}})";

    BybitRestOrderTransport tx(
        [post](std::string_view ep, std::string_view b) { return (*post)(ep, b); });

    auto r = tx.cancel("/v5/order/cancel",
                       R"({"category":"linear","symbol":"BTCUSDT","orderId":"42"})");

    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.exchange_order_id, "42");
    EXPECT_EQ(post->last_endpoint, "/v5/order/cancel");
    EXPECT_EQ(post->last_body,
              R"({"category":"linear","symbol":"BTCUSDT","orderId":"42"})");
    EXPECT_EQ(post->calls, 1); // single post path — no separate del
}

TEST(BybitRestOrderTransport, RetCodeNotZeroFails)
{
    auto post = std::make_shared<fake_post>();
    post->status = 200; // HTTP ok, business fail — critical Bybit quirk
    post->body =
        R"({"retCode":110007,"retMsg":"ab not enough for new order","result":{},"time":1})";

    BybitRestOrderTransport tx(
        [post](std::string_view ep, std::string_view b) { return (*post)(ep, b); });

    auto r = tx.submit("/v5/order/create", "{}");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.exchange_order_id.empty());
    EXPECT_NE(r.error.find("110007"), std::string::npos);
}

TEST(BybitRestOrderTransport, HttpNon2xxFails)
{
    auto post = std::make_shared<fake_post>();
    post->status = 400;
    post->body   = R"({"retCode":10001,"retMsg":"bad"})";

    BybitRestOrderTransport tx(
        [post](std::string_view ep, std::string_view b) { return (*post)(ep, b); });

    auto r = tx.submit("/v5/order/create", "{}");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("HTTP 400"), std::string::npos);
}

TEST(BybitRestOrderTransport, NullPostCallableReturnsError)
{
    BybitRestOrderTransport tx({});
    auto s = tx.submit("/v5/order/create", "{}");
    auto c = tx.cancel("/v5/order/cancel", "{}");
    EXPECT_FALSE(s.ok);
    EXPECT_FALSE(c.ok);
    EXPECT_FALSE(s.error.empty());
    EXPECT_FALSE(c.error.empty());
}

TEST(BybitRestOrderTransport, OpenCloseAreNoOps)
{
    BybitRestOrderTransport tx({});
    EXPECT_TRUE(tx.open());
    tx.close();
}

TEST(BybitRestOrderTransport, NumericOrderIdExtracted)
{
    auto post = std::make_shared<fake_post>();
    post->status = 200;
    post->body =
        R"({"retCode":0,"retMsg":"OK","result":{"orderId":121211212122}})";

    BybitRestOrderTransport tx(
        [post](std::string_view ep, std::string_view b) { return (*post)(ep, b); });

    auto r = tx.submit("/v5/order/create", "{}");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.exchange_order_id, "121211212122");
}

#endif // HAS_BYBIT
