#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_futures_order_encoder.h"

#include <chrono>
#include <string>

namespace {

static auto now() { return std::chrono::system_clock::now(); }

order_event make_order(const std::string& symbol,
                       order_type type,
                       order_side side,
                       double qty,
                       double price = 0.0,
                       time_in_force tif = time_in_force::gtc)
{
    order_event o(now(), symbol, type, side, qty, price, tif);
    o.set_order_id(1);
    return o;
}

} // namespace

TEST(BybitFuturesOrderEncoder, SubmitLimitExactJson)
{
    BybitFuturesOrderEncoder enc;
    auto o = make_order("btcusdt", order_type::limit, order_side::buy,
                        0.001, 50000.0, time_in_force::gtc);

    auto e = enc.encode_submit(o, "tt-1");

    EXPECT_EQ(e.endpoint, "/v5/order/create");
    EXPECT_EQ(e.client_order_id, "tt-1");
    EXPECT_EQ(e.wire_payload,
              R"({"category":"linear","symbol":"BTCUSDT","side":"Buy","orderType":"Limit","qty":"0.001","price":"50000","timeInForce":"GTC","positionIdx":0,"orderLinkId":"tt-1"})");
}

TEST(BybitFuturesOrderEncoder, SubmitMarketExactJson)
{
    BybitFuturesOrderEncoder enc;
    auto o = make_order("btcusdt", order_type::market, order_side::sell,
                        0.001);

    auto e = enc.encode_submit(o, "tt-m");

    EXPECT_EQ(e.endpoint, "/v5/order/create");
    EXPECT_EQ(e.wire_payload,
              R"({"category":"linear","symbol":"BTCUSDT","side":"Sell","orderType":"Market","qty":"0.001","positionIdx":0,"orderLinkId":"tt-m"})");
    // Market must not carry price / TIF
    EXPECT_EQ(e.wire_payload.find("price"), std::string::npos);
    EXPECT_EQ(e.wire_payload.find("timeInForce"), std::string::npos);
}

TEST(BybitFuturesOrderEncoder, CancelByOrderIdExactJson)
{
    BybitFuturesOrderEncoder enc;
    auto e = enc.encode_cancel("BTCUSDT", "189918", "tt-1");

    EXPECT_EQ(e.endpoint, "/v5/order/cancel");
    EXPECT_EQ(e.wire_payload,
              R"({"category":"linear","symbol":"BTCUSDT","orderId":"189918"})");
    // Prefer exchange id — orderLinkId must not appear
    EXPECT_EQ(e.wire_payload.find("orderLinkId"), std::string::npos);
}

TEST(BybitFuturesOrderEncoder, CancelByOrderLinkIdExactJson)
{
    BybitFuturesOrderEncoder enc;
    auto e = enc.encode_cancel("BTCUSDT", "", "tt-cli-1");

    EXPECT_EQ(e.endpoint, "/v5/order/cancel");
    EXPECT_EQ(e.wire_payload,
              R"({"category":"linear","symbol":"BTCUSDT","orderLinkId":"tt-cli-1"})");
}

TEST(BybitFuturesOrderEncoder, StopTypesRefuseEncode)
{
    BybitFuturesOrderEncoder enc;
    auto stop = make_order("BTCUSDT", order_type::stop, order_side::sell,
                           1.0, 0.0);
    auto sl   = make_order("BTCUSDT", order_type::stop_limit, order_side::buy,
                           1.0, 51000.0);

    auto e1 = enc.encode_submit(stop, "tt-s");
    auto e2 = enc.encode_submit(sl, "tt-sl");

    EXPECT_TRUE(e1.endpoint.empty());
    EXPECT_TRUE(e1.wire_payload.empty());
    EXPECT_TRUE(e2.endpoint.empty());
    EXPECT_TRUE(e2.wire_payload.empty());
}

TEST(BybitFuturesOrderEncoder, ReduceOnlyTrue)
{
    BybitFuturesOrderEncoder enc;
    enc.set_reduce_only(true);
    auto o = make_order("BTCUSDT", order_type::market, order_side::sell, 0.01);
    auto e = enc.encode_submit(o, "tt-ro");
    EXPECT_NE(e.wire_payload.find(R"("reduceOnly":true)"), std::string::npos);
}

TEST(BybitFuturesOrderEncoder, TifMappingUppercase)
{
    BybitFuturesOrderEncoder enc;
    auto ioc = enc.encode_submit(
        make_order("A", order_type::limit, order_side::buy, 1, 1, time_in_force::ioc),
        "c1");
    auto fok = enc.encode_submit(
        make_order("A", order_type::limit, order_side::buy, 1, 1, time_in_force::fok),
        "c2");
    auto day = enc.encode_submit(
        make_order("A", order_type::limit, order_side::buy, 1, 1, time_in_force::day),
        "c3");

    EXPECT_NE(ioc.wire_payload.find(R"("timeInForce":"IOC")"), std::string::npos);
    EXPECT_NE(fok.wire_payload.find(R"("timeInForce":"FOK")"), std::string::npos);
    EXPECT_NE(day.wire_payload.find(R"("timeInForce":"GTC")"), std::string::npos);
}

TEST(BybitFuturesOrderEncoder, DefaultSymbolWhenEmpty)
{
    BybitFuturesOrderEncoder enc("ethusdt");
    auto o = make_order("", order_type::limit, order_side::buy, 1.0, 20.0);
    auto e = enc.encode_submit(o, "tt-7");
    EXPECT_NE(e.wire_payload.find(R"("symbol":"ETHUSDT")"), std::string::npos);
}

TEST(BybitFuturesOrderEncoder, InvalidOrderLinkIdRefuses)
{
    BybitFuturesOrderEncoder enc;
    auto o = make_order("BTCUSDT", order_type::limit, order_side::buy, 1, 1);
    // space is illegal
    auto e = enc.encode_submit(o, "bad id");
    EXPECT_TRUE(e.endpoint.empty());
    EXPECT_TRUE(e.wire_payload.empty());

    EXPECT_FALSE(BybitFuturesOrderEncoder::valid_order_link_id(""));
    EXPECT_FALSE(BybitFuturesOrderEncoder::valid_order_link_id(
        std::string(37, 'a')));
    EXPECT_TRUE(BybitFuturesOrderEncoder::valid_order_link_id("tt-1.A:z/0_"));
}

TEST(BybitFuturesOrderEncoder, AlwaysPositionIdxZero)
{
    BybitFuturesOrderEncoder enc;
    auto e = enc.encode_submit(
        make_order("BTCUSDT", order_type::market, order_side::buy, 0.01),
        "tt-p");
    EXPECT_NE(e.wire_payload.find(R"("positionIdx":0)"), std::string::npos);
}

#endif // HAS_BYBIT
