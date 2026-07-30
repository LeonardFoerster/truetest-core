#include <gtest/gtest.h>

#include "providers/binance/binance_user_data_parser.h"

#include <string>

namespace {

// Minimal executionReport-shaped payload. Binance's real format uses many more
// fields - the parser only needs these.
std::string report(const std::string& x, const std::string& X,
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
    std::string j = R"({"e":"executionReport",)";
    j += R"("E":1700000000000,)";
    j += R"("s":"BTCUSDT",)";
    j += R"("c":")" + c + R"(",)";
    j += R"("S":")" + S + R"(",)";
    j += R"("x":")" + x + R"(",)";
    j += R"("X":")" + X + R"(",)";
    j += R"("r":")" + r + R"(",)";
    j += R"("i":)" + i + ",";
    j += R"("l":")" + l + R"(",)";
    j += R"("L":")" + L + R"(",)";
    j += R"("z":")" + z + R"(",)";
    j += R"("n":")" + n + R"(",)";
    j += R"("N":")" + N + R"("})";
    return j;
}

}

TEST(BinanceUserDataParser, RejectsNonExecutionReports)
{
    BinanceUserDataParser p;
    parsed_exec out;

    std::string bal = R"({"e":"outboundAccountPosition","E":1})";
    EXPECT_FALSE(p.parse(bal, out));

    std::string listen = R"({"e":"listenKeyExpired","E":1})";
    EXPECT_FALSE(p.parse(listen, out));
}

TEST(BinanceUserDataParser, NewAck)
{
    BinanceUserDataParser p;
    parsed_exec out;
    auto j = report("NEW", "NEW");

    ASSERT_TRUE(p.parse(j, out));
    EXPECT_EQ(out.k, parsed_exec::kind::ack);
    EXPECT_EQ(out.symbol, "BTCUSDT");
    EXPECT_EQ(out.client_order_id, "tt-1");
    EXPECT_EQ(out.exchange_order_id, "42");
    EXPECT_EQ(out.side, order_side::buy);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.0);
}

TEST(BinanceUserDataParser, PartialTrade)
{
    BinanceUserDataParser p;
    parsed_exec out;
    auto j = report("TRADE", "PARTIALLY_FILLED",
                    "0.4", "60000.0", "0.4");

    ASSERT_TRUE(p.parse(j, out));
    EXPECT_EQ(out.k, parsed_exec::kind::partial_fill);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.4);
    EXPECT_DOUBLE_EQ(out.last_fill_price, 60000.0);
    EXPECT_DOUBLE_EQ(out.cumulative_qty, 0.4);
}

TEST(BinanceUserDataParser, FullTrade)
{
    BinanceUserDataParser p;
    parsed_exec out;
    auto j = report("TRADE", "FILLED",
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

TEST(BinanceUserDataParser, Canceled)
{
    BinanceUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(report("CANCELED", "CANCELED"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::canceled);
}

TEST(BinanceUserDataParser, Rejected)
{
    BinanceUserDataParser p;
    parsed_exec out;
    auto j = report("REJECTED", "REJECTED", "0", "0", "0",
                    "tt-2", "BUY", "0", "0", "USDT",
                    "INSUFFICIENT_BALANCE");
    ASSERT_TRUE(p.parse(j, out));
    EXPECT_EQ(out.k, parsed_exec::kind::rejected);
    EXPECT_EQ(out.error, "INSUFFICIENT_BALANCE");
}

TEST(BinanceUserDataParser, Expired)
{
    BinanceUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(report("EXPIRED", "EXPIRED"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::expired);
}

TEST(BinanceUserDataParser, TimestampFromEventMillis)
{
    BinanceUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(report("NEW", "NEW"), out));

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  out.ts.time_since_epoch()).count();
    EXPECT_EQ(ms, 1700000000000LL);
}

TEST(BinanceUserDataParser, StringExchangeIdFallback)
{
    BinanceUserDataParser p;
    parsed_exec out;
    std::string j = R"({"e":"executionReport","E":1,"s":"X","c":"c1","S":"BUY",)"
                    R"("x":"NEW","X":"NEW","i":"alpha-7",)"
                    R"("l":"0","L":"0","z":"0","n":"0","N":"USDT"})";
    ASSERT_TRUE(p.parse(j, out));
    EXPECT_EQ(out.exchange_order_id, "alpha-7");
}
