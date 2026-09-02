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
    EXPECT_EQ(ticks[0].quantity_scale, 100'000'000ULL);
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
    EXPECT_EQ(std::get<provider::tick>(events[0]).quantity_scale, 100'000'000ULL);
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

TEST(BitunixParser, RejectsUnsafeTradeQuantityBeforeIntegerConversion)
{
    for (const char* qty : {"0", "-0", "-1", "1e-8", "nan", "inf",
                            "0.000000001", "0.123456789",
                            "92233720368.54775808"})
    {
        const std::string frame =
            std::string(R"({"ch":"trade","symbol":"BTCUSDT","ts":1,)"
                        R"("data":[{"p":"100","v":")")
            + qty + R"(","s":"buy"}]})";
        EXPECT_TRUE(bitunix::parse_all_trades(frame).empty()) << qty;
    }
}

TEST(BitunixParser, TradeQuantityPreservesOneAtomAndLargeAtomIdentity)
{
    const char* frame = R"({
      "ch":"trade","symbol":"BTCUSDT","ts":1,
      "data":[
        {"p":"100","v":"0.00000001","s":"buy"},
        {"p":"101","v":"90071992.54740993","s":"sell"}
      ]
    })";
    const auto trades = bitunix::parse_all_trades(frame);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].quantity, 1);
    EXPECT_EQ(trades[1].quantity, 9'007'199'254'740'993LL);
}

TEST(BitunixParser, TradeBatchIsTransactional)
{
    const char* frame = R"({
      "ch":"trade","symbol":"BTCUSDT","ts":1,
      "data":[
        {"p":"100","v":"1","s":"buy"},
        {"p":"101","v":"0.000000001","s":"sell"},
        {"p":"102","v":"1","s":"buy"}
      ]
    })";
    EXPECT_TRUE(bitunix::parse_all_trades(frame).empty());
    EXPECT_FALSE(bitunix::parse_trade_first(frame).has_value());

    BitunixCombinedParser parser;
    EXPECT_TRUE(parser.parse_records(frame).empty());
}

TEST(BitunixParser, RejectsCausallyReversedOrFutureTradeBatch)
{
    const char* descending = R"({
      "ch":"trade","symbol":"BTCUSDT","ts":1700000001000,
      "data":[
        {"p":"100","v":"1","s":"buy","t":"2023-11-14T22:13:20.900Z"},
        {"p":"101","v":"1","s":"sell","t":"2023-11-14T22:13:20.800Z"}
      ]
    })";
    const char* future = R"({
      "ch":"trade","symbol":"BTCUSDT","ts":1700000000000,
      "data":[
        {"p":"100","v":"1","s":"buy","t":"2023-11-14T22:13:21Z"}
      ]
    })";
    EXPECT_TRUE(bitunix::parse_all_trades(descending).empty());
    EXPECT_TRUE(bitunix::parse_all_trades(future).empty());
}

TEST(BitunixParser, StatefulAdaptersRejectCrossFrameTimeRegression)
{
    const char* later = R"({
      "ch":"trade","symbol":"BTCUSDT","ts":1700000001000,
      "data":[{"p":"100","v":"1","s":"buy","t":"2023-11-14T22:13:20.900Z"}]
    })";
    const char* earlier = R"({
      "ch":"trade","symbol":"BTCUSDT","ts":1700000001000,
      "data":[{"p":"99","v":"1","s":"sell","t":"2023-11-14T22:13:20.800Z"}]
    })";

    BitunixTradeParser trades;
    ASSERT_EQ(trades.parse_records(later).size(), 1u);
    EXPECT_TRUE(trades.parse_records(earlier).empty());

    BitunixCombinedParser combined;
    ASSERT_EQ(combined.parse_records(later).size(), 1u);
    EXPECT_TRUE(combined.parse_records(earlier).empty());
    EXPECT_EQ(combined.classify_empty_frame(earlier),
              empty_parse_status::malformed);
}

TEST(BitunixParser, StatefulAdaptersRejectKnownAtRegression)
{
    const char* first = R"({
      "ch":"trade","symbol":"BTCUSDT","ts":1700000002000,
      "data":[{"p":"100","v":"1","s":"buy","t":"2023-11-14T22:13:20.900Z"}]
    })";
    const char* regressed_known_at = R"({
      "ch":"trade","symbol":"BTCUSDT","ts":1700000001500,
      "data":[{"p":"101","v":"1","s":"buy","t":"2023-11-14T22:13:21.000Z"}]
    })";

    BitunixTradeParser trades;
    ASSERT_EQ(trades.parse_records(first).size(), 1u);
    EXPECT_TRUE(trades.parse_records(regressed_known_at).empty());

    BitunixCombinedParser combined;
    ASSERT_EQ(combined.parse_records(first).size(), 1u);
    EXPECT_TRUE(combined.parse_records(regressed_known_at).empty());
}

TEST(BitunixParser, RejectsMissingInvalidOrAmbiguousTimeWithoutWallclockFallback)
{
    for (const char* time_fields : {
             "",
             R"(,"ts":0)",
             R"(,"ts":-1)",
             R"(,"ts":1.5)",
             R"(,"ts":1,"ts":2)",
         }) {
        const std::string frame =
            std::string{R"({"ch":"trade","symbol":"BTCUSDT")"}
            + time_fields
            + R"(,"data":[{"p":"100","v":"1","s":"buy"}]})";
        EXPECT_TRUE(bitunix::parse_all_trades(frame).empty()) << time_fields;
    }

    EXPECT_TRUE(bitunix::parse_all_trades(
        R"({"ch":"trade","symbol":"BTCUSDT","data":[{"p":"100","v":"1","s":"buy","t":"2024-01-01T00:00:00Z"}]})")
                    .empty());

    for (const char* event_time : {
             "2024-02-30T00:00:00Z", "2024-01-01T24:00:00Z",
             "2024-01-01T00:00:00junk", "0", "9223372036854775808",
         }) {
        const std::string frame =
            std::string{R"({"ch":"trade","symbol":"BTCUSDT","ts":1,"data":[{"p":"100","v":"1","s":"buy","t":")"}
            + event_time + R"("}]})";
        EXPECT_TRUE(bitunix::parse_all_trades(frame).empty()) << event_time;
    }
}

TEST(BitunixParser, RejectsUnknownSideMissingSymbolAndNestedAuthoritySpoof)
{
    EXPECT_TRUE(bitunix::parse_all_trades(
        R"({"ch":"trade","symbol":"BTCUSDT","ts":1,"data":[{"p":"100","v":"1","s":"hold"}]})")
                    .empty());
    EXPECT_TRUE(bitunix::parse_all_trades(
        R"({"ch":"trade","ts":1,"data":[{"symbol":"BTCUSDT","p":"100","v":"1","s":"buy"}]})")
                    .empty());
    EXPECT_TRUE(bitunix::parse_all_trades(
        R"({"ch":"trade","ch":"tickers","symbol":"BTCUSDT","ts":1,"data":[{"p":"100","v":"1","s":"buy"}]})")
                    .empty());
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
    EXPECT_EQ(parser.classify_empty_frame(
                  R"({"op":"subscribe","code":0})"),
              empty_parse_status::ignored);
    EXPECT_EQ(parser.classify_empty_frame(
                  R"({"op":"subscribe","code":-1,"msg":"denied"})"),
              empty_parse_status::malformed);
    EXPECT_EQ(parser.classify_empty_frame(
                  R"({"ch":"trade","op":"pong"})"),
              empty_parse_status::malformed);
}

#else

TEST(BitunixParser, SkippedWithoutHasBitunix)
{
    GTEST_SKIP() << "HAS_BITUNIX not defined";
}

#endif // HAS_BITUNIX
