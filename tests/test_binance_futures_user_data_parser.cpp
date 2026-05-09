#include <gtest/gtest.h>

#include "providers/binance/binance_futures_user_data_parser.h"

#include <string>

namespace {

// Minimal ORDER_TRADE_UPDATE payload — wrapper carries event time `E`,
// inner `o:{...}` carries everything else. Real futures payloads have
// many more fields; the parser only needs these.
std::string update(const std::string& x, const std::string& X,
                   const std::string& l = "0.0",
                   const std::string& L = "0.0",
                   const std::string& z = "0.0",
                   const std::string& c = "tt-1",
                   const std::string& S = "BUY",
                   const std::string& i = "42",
                   const std::string& n = "0.0",
                   const std::string& N = "USDT",
                   const std::string& r = "NONE")
{
    std::string j = R"({"e":"ORDER_TRADE_UPDATE",)";
    j += R"("E":1700000000000,)";
    j += R"("T":1700000000000,)";
    j += R"("o":{)";
    j +=     R"("s":"BTCUSDT",)";
    j +=     R"("c":")" + c + R"(",)";
    j +=     R"("S":")" + S + R"(",)";
    j +=     R"("o":"LIMIT",)";  // inner order TYPE — must not collide with wrapper "o"
    j +=     R"("x":")" + x + R"(",)";
    j +=     R"("X":")" + X + R"(",)";
    j +=     R"("r":")" + r + R"(",)";
    j +=     R"("i":)" + i + ",";
    j +=     R"("l":")" + l + R"(",)";
    j +=     R"("L":")" + L + R"(",)";
    j +=     R"("z":")" + z + R"(",)";
    j +=     R"("n":")" + n + R"(",)";
    j +=     R"("N":")" + N + R"(",)";
    j +=     R"("T":1700000000001)";  // inner transaction time, distinct from wrapper E
    j += R"(}})";
    return j;
}

} // namespace

TEST(BinanceFuturesUserDataParser, RejectsNonOrderTradeUpdates)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;

    std::string acct = R"({"e":"ACCOUNT_UPDATE","E":1})";
    EXPECT_FALSE(p.parse(acct, out));

    std::string listen = R"({"e":"listenKeyExpired","E":1})";
    EXPECT_FALSE(p.parse(listen, out));

    // Spot's executionReport must NOT be accepted by the futures parser.
    std::string spot = R"({"e":"executionReport","E":1,"s":"X","c":"c","S":"BUY",)"
                       R"("x":"NEW","X":"NEW","i":1,"l":"0","L":"0","z":"0","n":"0","N":"USDT"})";
    EXPECT_FALSE(p.parse(spot, out));
}

TEST(BinanceFuturesUserDataParser, NewAck)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    auto j = update("NEW", "NEW");

    ASSERT_TRUE(p.parse(j, out));
    EXPECT_EQ(out.k, parsed_exec::kind::ack);
    EXPECT_EQ(out.symbol, "BTCUSDT");
    EXPECT_EQ(out.client_order_id, "tt-1");
    EXPECT_EQ(out.exchange_order_id, "42");
    EXPECT_EQ(out.side, order_side::buy);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.0);
}

TEST(BinanceFuturesUserDataParser, PartialTrade)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    auto j = update("TRADE", "PARTIALLY_FILLED",
                    "0.4", "60000.0", "0.4");

    ASSERT_TRUE(p.parse(j, out));
    EXPECT_EQ(out.k, parsed_exec::kind::partial_fill);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.4);
    EXPECT_DOUBLE_EQ(out.last_fill_price, 60000.0);
    EXPECT_DOUBLE_EQ(out.cumulative_qty, 0.4);
}

TEST(BinanceFuturesUserDataParser, FullTrade)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    auto j = update("TRADE", "FILLED",
                    "0.6", "60010.0", "1.0",
                    "tt-9", "SELL", "77",
                    "0.06", "USDT");

    ASSERT_TRUE(p.parse(j, out));
    EXPECT_EQ(out.k, parsed_exec::kind::full_fill);
    EXPECT_EQ(out.side, order_side::sell);
    EXPECT_EQ(out.client_order_id, "tt-9");
    EXPECT_EQ(out.exchange_order_id, "77");
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.6);
    EXPECT_DOUBLE_EQ(out.last_fill_price, 60010.0);
    EXPECT_DOUBLE_EQ(out.cumulative_qty, 1.0);
    EXPECT_DOUBLE_EQ(out.commission, 0.06);
    EXPECT_EQ(out.commission_asset, "USDT");
}

TEST(BinanceFuturesUserDataParser, Canceled)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(update("CANCELED", "CANCELED"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::canceled);
}

TEST(BinanceFuturesUserDataParser, Rejected)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    auto j = update("REJECTED", "REJECTED", "0", "0", "0",
                    "tt-2", "BUY", "0", "0", "USDT",
                    "INSUFFICIENT_MARGIN");
    ASSERT_TRUE(p.parse(j, out));
    EXPECT_EQ(out.k, parsed_exec::kind::rejected);
    EXPECT_EQ(out.error, "INSUFFICIENT_MARGIN");
}

TEST(BinanceFuturesUserDataParser, Expired)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(update("EXPIRED", "EXPIRED"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::expired);
}

TEST(BinanceFuturesUserDataParser, TimestampFromWrapperEventMillis)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(update("NEW", "NEW"), out));

    // Wrapper `E`, not inner `T`, drives the timestamp (matches spot).
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  out.ts.time_since_epoch()).count();
    EXPECT_EQ(ms, 1700000000000LL);
}

TEST(BinanceFuturesUserDataParser, StringExchangeIdFallback)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    std::string j = R"({"e":"ORDER_TRADE_UPDATE","E":1,"o":{)"
                    R"("s":"X","c":"c1","S":"BUY","x":"NEW","X":"NEW",)"
                    R"("i":"alpha-7","l":"0","L":"0","z":"0","n":"0","N":"USDT"}})";
    ASSERT_TRUE(p.parse(j, out));
    EXPECT_EQ(out.exchange_order_id, "alpha-7");
}
