#include <gtest/gtest.h>

#ifdef HAS_BITUNIX

#include "providers/bitunix/bitunix_parser.h"

#include <variant>

namespace {

const char* kTradeFrame = R"({
  "ch": "trade",
  "symbol": "BTCUSDT",
  "ts": 1700000000123,
  "data": [
    {"t": "2023-11-14T22:13:20Z", "p": "68621.4", "v": "0.7142", "s": "buy"},
    {"t": "2023-11-14T22:13:20Z", "p": "68621.5", "v": "0.0018", "s": "sell"}
  ]
})";

} // namespace

TEST(BitunixParser, ParseAllTradesBatch)
{
    auto ticks = bitunix::parse_all_trades(kTradeFrame);
    ASSERT_EQ(ticks.size(), 2u);
    EXPECT_EQ(ticks[0].symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(ticks[0].price, 68621.4);
    // Quantity is fixed-scale int64 (×1e8), matching Binance/Bitget domain Tick.
    EXPECT_EQ(ticks[0].quantity, static_cast<std::int64_t>(0.7142 * 1e8));
    EXPECT_EQ(ticks[0].side, data_tick_side::bid);
    EXPECT_DOUBLE_EQ(ticks[1].price, 68621.5);
    EXPECT_EQ(ticks[1].quantity, static_cast<std::int64_t>(0.0018 * 1e8));
    EXPECT_EQ(ticks[1].side, data_tick_side::ask);
}

TEST(BitunixParser, CombinedParserEmitsEveryTrade)
{
    BitunixCombinedParser parser;
    auto events = parser.parse_records(kTradeFrame);
    ASSERT_EQ(events.size(), 2u);
    ASSERT_TRUE(std::holds_alternative<provider::tick>(events[0]));
    ASSERT_TRUE(std::holds_alternative<provider::tick>(events[1]));
    EXPECT_DOUBLE_EQ(std::get<provider::tick>(events[0]).price, 68621.4);
    EXPECT_DOUBLE_EQ(std::get<provider::tick>(events[1]).price, 68621.5);
}

TEST(BitunixParser, IgnoresPongAndNonTrade)
{
    BitunixCombinedParser parser;
    EXPECT_TRUE(parser.parse_records(R"({"op":"pong","pong":1})").empty());
    EXPECT_TRUE(parser.parse_records(
        R"({"ch":"tickers","symbol":"BTCUSDT","data":{}})").empty());
}

TEST(BitunixParser, EmptyDataYieldsNothing)
{
    BitunixCombinedParser parser;
    EXPECT_TRUE(parser.parse_records(
        R"({"ch":"trade","symbol":"BTCUSDT","ts":1,"data":[]})").empty());
}

TEST(BitunixParser, ControlAckRequiresAuthoritativeUniqueEnvelope)
{
    BitunixCombinedParser parser;
    EXPECT_EQ(parser.classify_empty_frame(R"({"op":"pong","pong":1})"),
              empty_parse_status::ignored);
    EXPECT_EQ(parser.classify_empty_frame(R"(garbage "op":"pong")"),
              empty_parse_status::malformed);
    EXPECT_EQ(parser.classify_empty_frame(
                  R"({"op":"pong","op":"subscribe"})"),
              empty_parse_status::malformed);
    EXPECT_EQ(parser.classify_empty_frame(R"({"op":"pong"} trailing)"),
              empty_parse_status::malformed);
}

#else

TEST(BitunixParser, SkippedWithoutHasBitunix)
{
    GTEST_SKIP() << "HAS_BITUNIX not defined";
}

#endif // HAS_BITUNIX
