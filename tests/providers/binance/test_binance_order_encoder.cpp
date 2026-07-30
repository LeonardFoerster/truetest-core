#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_order_encoder.h"

#include <chrono>
#include <string>

namespace {

static auto now() { return std::chrono::system_clock::now(); }

order_event make_order(const std::string& symbol,
                       order_type type,
                       order_side side,
                       double qty,
                       double price = 0.0,
                       time_in_force tif = time_in_force::gtc,
                       double stop = 0.0)
{
    order_event o(now(), symbol, type, side, qty, price, tif, stop);
    o.set_order_id(1);
    return o;
}

}

TEST(BinanceOrderEncoder, SubmitLimitBuildsSignedParamsShape)
{
    BinanceOrderEncoder enc;
    auto o = make_order("btcusdt", order_type::limit, order_side::buy,
                        1.5, 50000.0, time_in_force::gtc);

    auto e = enc.encode_submit(o, "tt-1");

    EXPECT_EQ(e.endpoint, "/api/v3/order");
    EXPECT_EQ(e.client_order_id, "tt-1");

    const auto& p = e.wire_payload;
    EXPECT_NE(p.find("symbol=BTCUSDT"), std::string::npos);
    EXPECT_NE(p.find("side=BUY"),       std::string::npos);
    EXPECT_NE(p.find("type=LIMIT"),     std::string::npos);
    EXPECT_NE(p.find("quantity="),      std::string::npos);
    EXPECT_NE(p.find("price="),         std::string::npos);
    EXPECT_NE(p.find("timeInForce=GTC"), std::string::npos);
    EXPECT_NE(p.find("newClientOrderId=tt-1"), std::string::npos);
}

TEST(BinanceOrderEncoder, SubmitMarketOmitsPriceAndTif)
{
    BinanceOrderEncoder enc;
    auto o = make_order("ethusdt", order_type::market, order_side::sell,
                        0.25, 0.0, time_in_force::ioc);

    auto e = enc.encode_submit(o, "tt-42");

    const auto& p = e.wire_payload;
    EXPECT_NE(p.find("type=MARKET"),  std::string::npos);
    EXPECT_NE(p.find("side=SELL"),    std::string::npos);
    EXPECT_EQ(p.find("price="),       std::string::npos);
    EXPECT_EQ(p.find("timeInForce="), std::string::npos);
}

TEST(BinanceOrderEncoder, SubmitStopLimitIncludesStopPrice)
{
    BinanceOrderEncoder enc;
    auto o = make_order("btcusdt", order_type::stop_limit, order_side::buy,
                        1.0, 101.0, time_in_force::gtc, 100.0);

    auto e = enc.encode_submit(o, "tt-9");
    const auto& p = e.wire_payload;

    EXPECT_NE(p.find("type=STOP_LOSS_LIMIT"), std::string::npos);
    EXPECT_NE(p.find("stopPrice="),           std::string::npos);
    EXPECT_NE(p.find("price="),               std::string::npos);
}

TEST(BinanceOrderEncoder, SubmitResolvesDefaultSymbolWhenEmpty)
{
    BinanceOrderEncoder enc("solusdt");
    auto o = make_order("", order_type::limit, order_side::buy,
                        1.0, 20.0);

    auto e = enc.encode_submit(o, "tt-7");
    EXPECT_NE(e.wire_payload.find("symbol=SOLUSDT"), std::string::npos);
}

TEST(BinanceOrderEncoder, SubmitTifMappingCoversAllVariants)
{
    BinanceOrderEncoder enc;

    auto ioc = enc.encode_submit(
        make_order("a", order_type::limit, order_side::buy, 1, 1, time_in_force::ioc),
        "c1");
    auto fok = enc.encode_submit(
        make_order("a", order_type::limit, order_side::buy, 1, 1, time_in_force::fok),
        "c2");
    auto day = enc.encode_submit(
        make_order("a", order_type::limit, order_side::buy, 1, 1, time_in_force::day),
        "c3");

    EXPECT_NE(ioc.wire_payload.find("timeInForce=IOC"), std::string::npos);
    EXPECT_NE(fok.wire_payload.find("timeInForce=FOK"), std::string::npos);
    EXPECT_NE(day.wire_payload.find("timeInForce=GTC"), std::string::npos);
}

TEST(BinanceOrderEncoder, CancelPrefersExchangeOrderId)
{
    BinanceOrderEncoder enc;
    auto e = enc.encode_cancel("btcusdt", "EX-123", "tt-5");

    EXPECT_EQ(e.endpoint, "/api/v3/order");
    const auto& p = e.wire_payload;
    EXPECT_NE(p.find("symbol=BTCUSDT"), std::string::npos);
    EXPECT_NE(p.find("orderId=EX-123"), std::string::npos);
    EXPECT_EQ(p.find("origClientOrderId="), std::string::npos);
}

TEST(BinanceOrderEncoder, CancelFallsBackToClientIdWhenExchangeEmpty)
{
    BinanceOrderEncoder enc;
    auto e = enc.encode_cancel("btcusdt", "", "tt-5");

    const auto& p = e.wire_payload;
    EXPECT_NE(p.find("origClientOrderId=tt-5"), std::string::npos);
    EXPECT_EQ(p.find("orderId="), std::string::npos);
}

TEST(BinanceOrderEncoder, CancelUsesDefaultSymbolWhenEmpty)
{
    BinanceOrderEncoder enc("btcusdt");
    auto e = enc.encode_cancel("", "EX-1", "tt-1");
    EXPECT_NE(e.wire_payload.find("symbol=BTCUSDT"), std::string::npos);
}

TEST(BinanceOrderEncoder, EncodesParameterValues)
{
    BinanceOrderEncoder enc;
    auto o = make_order("btc&usdt", order_type::limit, order_side::buy,
                        1.0, 10.0);

    auto submit = enc.encode_submit(o, "tt&x=y");
    EXPECT_NE(submit.wire_payload.find("symbol=BTC%26USDT"), std::string::npos);
    EXPECT_NE(submit.wire_payload.find("newClientOrderId=tt%26x%3d"), std::string::npos);
    EXPECT_EQ(submit.wire_payload.find("newClientOrderId=tt&x=y"), std::string::npos);

    auto cancel = enc.encode_cancel("btc&usdt", "", "tt&x=y");
    EXPECT_NE(cancel.wire_payload.find("symbol=BTC%26USDT"), std::string::npos);
    EXPECT_NE(cancel.wire_payload.find("origClientOrderId=tt%26x%3d"), std::string::npos);
}

TEST(BinanceLogRedaction, RedactsSensitiveJsonFields)
{
    const std::string body =
        R"({"code":-1,"listenKey":"abc123","signature":"deadbeef","msg":"bad"})";

    const auto redacted = binance::redact_for_log(body, 240);
    EXPECT_EQ(redacted.find("abc123"), std::string::npos);
    EXPECT_EQ(redacted.find("deadbeef"), std::string::npos);
    EXPECT_NE(redacted.find("listenKey"), std::string::npos);
    EXPECT_NE(redacted.find("signature"), std::string::npos);
    EXPECT_NE(redacted.find("<redacted>"), std::string::npos);
}

TEST(BinanceLogRedaction, RedactsSensitiveParamsAndBearer)
{
    const std::string body =
        "listenKey=abc123&symbol=BTCUSDT&signature=deadbeef "
        "Authorization: Bearer topsecret";

    const auto redacted = binance::redact_for_log(body, 240);
    EXPECT_EQ(redacted.find("abc123"), std::string::npos);
    EXPECT_EQ(redacted.find("deadbeef"), std::string::npos);
    EXPECT_EQ(redacted.find("topsecret"), std::string::npos);
    EXPECT_NE(redacted.find("symbol=BTCUSDT"), std::string::npos);
}

#endif // HAS_BINANCE
