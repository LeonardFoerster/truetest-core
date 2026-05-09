#include <gtest/gtest.h>
#include "providers/binance/binance_parser.h"

// --- JSON extraction tests ---

TEST(BinanceParser, ExtractString)
{
    std::string json = R"({"e":"trade","s":"BTCUSDT","p":"50000.50"})";
    EXPECT_EQ(binance::extract_string(json, "e"), "trade");
    EXPECT_EQ(binance::extract_string(json, "s"), "BTCUSDT");
    EXPECT_EQ(binance::extract_string(json, "p"), "50000.50");
    EXPECT_EQ(binance::extract_string(json, "missing"), "");
}

TEST(BinanceParser, ExtractNumber)
{
    std::string json = R"({"T":1234567890,"n":100})";
    EXPECT_EQ(binance::extract_number(json, "T"), "1234567890");
    EXPECT_EQ(binance::extract_number(json, "n"), "100");
    EXPECT_EQ(binance::extract_number(json, "missing"), "");
}

TEST(BinanceParser, ExtractQuotedNumber)
{
    std::string json = R"({"p":"50000.50","q":"1.5"})";
    EXPECT_EQ(binance::extract_number(json, "p"), "50000.50");
    EXPECT_EQ(binance::extract_number(json, "q"), "1.5");
}

TEST(BinanceParser, ExtractBool)
{
    std::string json = R"({"m":true,"M":false})";
    EXPECT_TRUE(binance::extract_bool(json, "m"));
    EXPECT_FALSE(binance::extract_bool(json, "M"));
    EXPECT_FALSE(binance::extract_bool(json, "missing"));
}

// Tightened parser: only an exact lowercase `true` is true; anything
// else (including substrings starting with 't' like "truncated", and
// uppercase "True") is treated as not-true. The previous behavior
// returned true on any value starting with 't', which mis-classified
// truncated WAF responses as boolean true on the safety gates.
TEST(BinanceParser, ExtractBoolRequiresExactTrue)
{
    EXPECT_TRUE(binance::extract_sv_bool(R"({"x":true})", "x"));
    EXPECT_FALSE(binance::extract_sv_bool(R"({"x":false})", "x"));
    EXPECT_FALSE(binance::extract_sv_bool(R"({"x":True})", "x"));
    EXPECT_FALSE(binance::extract_sv_bool(R"({"x":truncated})", "x"));
    EXPECT_FALSE(binance::extract_sv_bool(R"({"x":tru})", "x"));
    EXPECT_FALSE(binance::extract_sv_bool(R"({"y":true})", "x"));
}

TEST(BinanceParser, ExtractOptionalBoolDistinguishesMissing)
{
    auto t = binance::extract_sv_optional_bool(R"({"x":true})", "x");
    ASSERT_TRUE(t.has_value());
    EXPECT_TRUE(*t);

    auto f = binance::extract_sv_optional_bool(R"({"x":false})", "x");
    ASSERT_TRUE(f.has_value());
    EXPECT_FALSE(*f);

    EXPECT_FALSE(binance::extract_sv_optional_bool(R"({"x":True})", "x").has_value());
    EXPECT_FALSE(binance::extract_sv_optional_bool(R"({"x":tru})", "x").has_value());
    EXPECT_FALSE(binance::extract_sv_optional_bool(R"({"y":true})", "x").has_value());
}

// --- Trade parsing tests ---

TEST(BinanceParser, ParseTradeMessage)
{
    std::string json = R"({
        "e":"trade","E":1672531200000,"s":"BTCUSDT","t":12345,
        "p":"16800.50","q":"0.5","b":88,"a":50,
        "T":1672531200100,"m":true,"M":true
    })";

    auto result = binance::parse_trade(json);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(result->price, 16800.50);
    EXPECT_EQ(result->quantity, 50000000);  // 0.5 * 1e8 (satoshi-scaled)
    EXPECT_EQ(result->side, data_tick_side::ask);  // m=true → seller aggressor → ask

    auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        result->timestamp.time_since_epoch()).count();
    EXPECT_EQ(ts_ms, 1672531200100);
}

TEST(BinanceParser, ParseTradeMessageBuyAggressor)
{
    std::string json = R"({
        "e":"trade","E":1672531200000,"s":"ETHUSDT",
        "p":"1200.00","q":"10","T":1672531200100,"m":false
    })";

    auto result = binance::parse_trade(json);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->symbol, "ETHUSDT");
    EXPECT_EQ(result->side, data_tick_side::bid);  // m=false → buyer aggressor → bid
    EXPECT_EQ(result->quantity, 1000000000);  // 10 * 1e8
}

TEST(BinanceParser, ParseTradeWrongEventType)
{
    std::string json = R"({"e":"kline","s":"BTCUSDT"})";
    auto result = binance::parse_trade(json);
    EXPECT_FALSE(result.has_value());
}

TEST(BinanceParser, ParseTradeMissingFields)
{
    std::string json = R"({"e":"trade","s":"BTCUSDT"})";
    auto result = binance::parse_trade(json);
    EXPECT_FALSE(result.has_value());
}

// --- Kline parsing tests ---

TEST(BinanceParser, ParseKlineMessage)
{
    std::string json = R"({
        "e":"kline","E":1672531200000,"s":"BTCUSDT",
        "k":{
            "t":1672531200000,"T":1672531259999,"s":"BTCUSDT","i":"1m",
            "o":"16800.00","c":"16850.50","h":"16860.00","l":"16790.00",
            "v":"100.5","n":500,"x":false
        }
    })";

    auto result = binance::parse_kline(json);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(result->open, 16800.00);
    EXPECT_DOUBLE_EQ(result->close, 16850.50);
    EXPECT_DOUBLE_EQ(result->high, 16860.00);
    EXPECT_DOUBLE_EQ(result->low, 16790.00);
    EXPECT_EQ(result->volume, 10050000000);  // 100.5 * 1e8 (satoshi-scaled)
}

TEST(BinanceParser, ParseKlineWrongEventType)
{
    std::string json = R"({"e":"trade","s":"BTCUSDT"})";
    auto result = binance::parse_kline(json);
    EXPECT_FALSE(result.has_value());
}

TEST(BinanceParser, ParseKlineMissingKObject)
{
    std::string json = R"({"e":"kline","s":"BTCUSDT"})";
    auto result = binance::parse_kline(json);
    EXPECT_FALSE(result.has_value());
}

// --- IDataParser adapter tests ---

TEST(BinanceParser, TradeParserAdapter)
{
    BinanceTradeParser parser;
    EXPECT_TRUE(parser.parse_header(""));  // no header needed

    std::string json = R"({"e":"trade","s":"BTCUSDT","p":"100.0","q":"5","T":1000,"m":false})";
    auto result = parser.parse_record(json);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol, "BTCUSDT");
}

TEST(BinanceParser, KlineParserAdapter)
{
    BinanceKlineParser parser;
    EXPECT_TRUE(parser.parse_header(""));

    std::string json = R"({"e":"kline","s":"ETHUSDT","k":{"t":1000,"s":"ETHUSDT","o":"100","c":"101","h":"102","l":"99","v":"50"}})";
    auto result = parser.parse_record(json);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol, "ETHUSDT");
    EXPECT_DOUBLE_EQ(result->close, 101.0);
}
