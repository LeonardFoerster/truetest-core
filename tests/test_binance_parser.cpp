#include <gtest/gtest.h>
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_replay_transport.h"

#include <filesystem>
#include <fstream>

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
        "e":"trade","E":1672531200200,"s":"BTCUSDT","t":12345,
        "p":"16800.50","q":"0.5","b":88,"a":50,
        "T":1672531200100,"m":true,"M":true
    })";

    auto result = binance::parse_trade(json);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(result->price, 16800.50);
    EXPECT_EQ(result->quantity, 50000000);  // 0.5 * 1e8 (satoshi-scaled)
    EXPECT_EQ(result->quantity_scale, 100'000'000ULL);
    EXPECT_EQ(result->side, data_tick_side::ask);  // m=true -> seller aggressor -> ask

    auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        result->timestamp.time_since_epoch()).count();
    EXPECT_EQ(ts_ms, 1672531200200)
        << "trade decisions use the authoritative known-at event time E";
}

TEST(BinanceParser, ParseTradeMessageBuyAggressor)
{
    std::string json = R"({
        "e":"trade","E":1672531200200,"s":"ETHUSDT","t":12346,
        "p":"1200.00","q":"10","T":1672531200100,"m":false
    })";

    auto result = binance::parse_trade(json);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->symbol, "ETHUSDT");
    EXPECT_EQ(result->side, data_tick_side::bid);  // m=false -> buyer aggressor -> bid
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

TEST(BinanceParser, C07_RejectsMissingOrInvalidTradeTimestamp)
{
    for (const char* timestamp : {
             static_cast<const char*>(nullptr), "0", "-1", "1x",
             "9223372036854775808"})
    {
        std::string json =
            R"({"e":"trade","E":2,"s":"BTCUSDT","t":1,"p":"100","q":"1","m":false)";
        if (timestamp)
        {
            json += R"(,"T":)";
            json += timestamp;
        }
        json += "}";
        EXPECT_FALSE(binance::parse_trade(json).has_value())
            << (timestamp ? timestamp : "missing");
    }
}

TEST(BinanceParser, RejectsUnsafeTradeQuantitiesBeforeIntegerConversion)
{
    for (const char* qty : {"0", "-0", "-1", "1e-8", "nan", "inf",
                            "0.000000001", "0.123456789",
                            "92233720368.54775808"})
    {
        const std::string json =
            std::string(R"({"e":"trade","E":2,"s":"BTCUSDT","t":1,"p":"100","q":")")
            + qty + R"(","T":1,"m":false})";
        EXPECT_FALSE(binance::parse_trade(json).has_value()) << qty;
    }
}

TEST(BinanceParser, TradeQuantityPreservesOneAtomExactly)
{
    const auto trade = binance::parse_trade(
        R"({"e":"trade","E":2,"s":"BTCUSDT","t":1,"p":"100","q":"0.00000001","T":1,"m":false})");
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->quantity, 1);
}

TEST(BinanceParser, TradeRejectsDuplicateOrMissingDecisionFields)
{
    EXPECT_FALSE(binance::parse_trade(
        R"({"e":"trade","s":"BTCUSDT","p":"100","q":"1","q":"2","T":1,"m":false})")
                     .has_value());
    EXPECT_FALSE(binance::parse_trade(
        R"({"e":"trade","s":"BTCUSDT","p":"100","q":"1","T":1})")
                     .has_value());
    EXPECT_FALSE(binance::parse_trade(
        R"({"e":"trade","s":"BTCUSDT","s":"ETHUSDT","p":"100","q":"1","T":1,"m":false})")
                     .has_value());
    EXPECT_FALSE(binance::parse_trade(
        R"({"e":"trade" "s":"BTCUSDT","p":"100","q":"1","T":1,"m":false})")
                     .has_value());
    EXPECT_FALSE(binance::parse_trade(
        R"({"e":"trade","s":"BTCUSDT","p":"100","q":"1","T":1,"m":false,})")
                     .has_value());
}

TEST(BinanceParser, TradeRequiresUniquePositiveNativeIdAndCausalEventTime)
{
    for (const char* id_field : {
             "", R"(,"t":0)", R"(,"t":-1)", R"(,"t":"x")",
             R"(,"t":1,"t":2)"})
    {
        const std::string json =
            std::string{R"({"e":"trade","E":2,"s":"BTCUSDT","p":"100","q":"1","T":1,"m":false)"}
            + id_field + "}";
        EXPECT_FALSE(binance::parse_trade(json).has_value()) << id_field;
    }
    EXPECT_FALSE(binance::parse_trade(
        R"({"e":"trade","E":1,"s":"BTCUSDT","t":1,"p":"100","q":"1","T":2,"m":false})")
                     .has_value());
}

TEST(BinanceParser, StatefulAdapterRejectsCrossFrameKnownAtRegression)
{
    BinanceTradeParser parser;
    ASSERT_TRUE(parser.parse_record(std::string_view{
        R"({"e":"trade","E":2,"s":"BTCUSDT","t":2,"p":"100","q":"1","T":1,"m":false})"})
                    .has_value());
    EXPECT_FALSE(parser.parse_record(std::string_view{
        R"({"e":"trade","E":1,"s":"BTCUSDT","t":1,"p":"99","q":"1","T":1,"m":false})"})
                     .has_value());
}

// --- Kline parsing tests ---

TEST(BinanceParser, ParseKlineMessage)
{
    std::string json = R"({
        "e":"kline","E":1672531260000,"s":"BTCUSDT",
        "k":{
            "t":1672531200000,"T":1672531259999,"s":"BTCUSDT","i":"1m",
            "o":"16800.00","c":"16850.50","h":"16860.00","l":"16790.00",
            "v":"100.5","n":500,"x":true
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
    EXPECT_EQ(result->quantity_scale, 100'000'000ULL);
    EXPECT_EQ(result->open_time_ms, 1672531200000LL);
}

TEST(BinanceParser, C02_CompletedKlineBecomesObservableAtCloseTime)
{
    const std::string json = R"({
        "e":"kline","E":1672531260001,"s":"BTCUSDT",
        "k":{
            "t":1672531200000,"T":1672531259999,"s":"BTCUSDT","i":"1m",
            "o":"16800","c":"16850","h":"16860","l":"16790",
            "v":"100.5","x":true
        }
    })";

    auto result = binance::parse_kline(json);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->date, "1672531260001")
        << "a completed candle uses authoritative known-at event time E";
}

TEST(BinanceParser, C14_CompletedKlineWithoutInstrumentIdentityFailsClosed)
{
    const std::string json = R"({
        "e":"kline","E":1672531259999,"s":"",
        "k":{
            "t":1672531200000,"T":1672531259999,"s":"","i":"1m",
            "o":"16800","c":"16850","h":"16860","l":"16790",
            "v":"100.5","x":true
        }
    })";

    EXPECT_FALSE(binance::parse_kline(json).has_value());
}

TEST(BinanceParser, C14_CompletedKlineWithConflictingInstrumentIdentityFailsClosed)
{
    const std::string json = R"({
        "e":"kline","E":1672531259999,"s":"BTCUSDT",
        "k":{
            "t":1672531200000,"T":1672531259999,"s":"ETHUSDT","i":"1m",
            "o":"16800","c":"16850","h":"16860","l":"16790",
            "v":"100.5","x":true
        }
    })";

    EXPECT_FALSE(binance::parse_kline(json).has_value());
}

TEST(BinanceParser, C02_RejectsInvalidCloseTimeAndMalformedOhlc)
{
    for (const char* kline_fields :
         {R"("t":1000,"s":"BTCUSDT","i":"1m","o":"10","c":"10","h":"11","l":"9")",
          R"("t":1000,"T":0,"s":"BTCUSDT","i":"1m","o":"10","c":"10","h":"11","l":"9")",
          R"("t":61000,"T":60999,"s":"BTCUSDT","i":"1m","o":"10","c":"10","h":"11","l":"9")",
          R"("t":1000,"T":60999,"s":"BTCUSDT","i":"1m","o":"10","c":"10","h":"9","l":"8")",
          R"("t":1000,"T":60999,"s":"BTCUSDT","i":"1m","o":"10","c":"10","h":"12","l":"11")",
          R"("t":1000,"T":60999,"s":"BTCUSDT","i":"1m","o":"10","c":"10","h":"9","l":"11")"}) {
        const std::string json =
            std::string{R"({"e":"kline","E":61000,"s":"BTCUSDT","k":{)"}
            + kline_fields + R"(,"v":"1","x":true}})";
        EXPECT_FALSE(binance::parse_kline(json).has_value()) << kline_fields;
    }
}

TEST(BinanceParser, EverySupportedFixedIntervalHasExactWireDuration)
{
    const std::pair<std::string_view, std::int64_t> intervals[] = {
        {"1s", 1'000}, {"1m", 60'000}, {"3m", 180'000},
        {"5m", 300'000}, {"15m", 900'000}, {"30m", 1'800'000},
        {"1h", 3'600'000}, {"2h", 7'200'000}, {"4h", 14'400'000},
        {"6h", 21'600'000}, {"8h", 28'800'000}, {"12h", 43'200'000},
        {"1d", 86'400'000}, {"3d", 259'200'000}, {"1w", 604'800'000},
    };
    constexpr std::int64_t open = 1'704'067'200'000;
    for (const auto& [name, duration] : intervals)
    {
        EXPECT_EQ(binance::fixed_kline_interval_ms(name),
                  std::optional<std::int64_t>{duration}) << name;
        EXPECT_TRUE(binance::kline_times_match_fixed_interval(
            open, open + duration - 1, name)) << name;
        EXPECT_FALSE(binance::kline_times_match_fixed_interval(
            open, open + duration - 2, name)) << name;
        EXPECT_FALSE(binance::kline_times_match_fixed_interval(
            open, open + duration, name)) << name;
    }
    EXPECT_FALSE(binance::fixed_kline_interval_ms("1M").has_value());
    EXPECT_FALSE(binance::fixed_kline_interval_ms("unknown").has_value());
}

TEST(BinanceParser, KlineIntervalMustExactlyMatchOpenAndCloseTimes)
{
    const auto frame = [](std::string_view interval,
                          std::int64_t close_time) {
        return std::string{R"({"e":"kline","E":1704067260000,"s":"BTCUSDT","k":{"t":1704067200000,"T":)"}
            + std::to_string(close_time)
            + R"(,"s":"BTCUSDT","i":")" + std::string{interval}
            + R"(","o":"1","h":"2","l":"0.5","c":"1.5","v":"1","x":true}})";
    };

    EXPECT_TRUE(binance::parse_kline(frame("1m", 1704067259999)).has_value());
    EXPECT_FALSE(binance::parse_kline(frame("1m", 1704067200001)).has_value());
    EXPECT_FALSE(binance::parse_kline(frame("1h", 1704067259999)).has_value());
    EXPECT_FALSE(binance::parse_kline(frame("1M", 1704067259999)).has_value())
        << "calendar intervals require an explicit calendar model";
}

TEST(BinanceParser, IgnoresUnfinishedKlineUpdate)
{
    const std::string json = R"({
        "e":"kline","s":"BTCUSDT","k":{
            "t":1672531200000,"s":"BTCUSDT",
            "o":"16800","c":"16850","h":"16860","l":"16790",
            "v":"100.5","x":false
        }
    })";

    EXPECT_FALSE(binance::parse_kline(json).has_value());
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

TEST(BinanceParser, RejectsUnsafeClosedKlineVolume)
{
    for (const char* volume : {"0", "-0", "-1", "1e-8", "nan", "inf",
                               "0.000000001", "0.123456789",
                               "92233720368.54775808"})
    {
        const std::string json =
            std::string(R"({"e":"kline","E":61000,"s":"BTCUSDT","k":{)"
                        R"("t":1000,"T":60999,"s":"BTCUSDT","i":"1m","o":"1","h":"2",)"
                        R"("l":"0.5","c":"1.5","v":")")
            + volume + R"(","x":true}})";
        EXPECT_FALSE(binance::parse_kline(json).has_value()) << volume;
    }
}

TEST(BinanceParser, ClosedKlineRequiresExplicitPositiveVolume)
{
    const char* missing =
        R"({"e":"kline","E":3,"s":"BTCUSDT","k":{"t":1,"T":2,"s":"BTCUSDT","o":"1","h":"2","l":"0.5","c":"1.5","x":true}})";
    EXPECT_FALSE(binance::parse_kline(missing).has_value());

    const char* one_atom =
        R"({"e":"kline","E":60000,"s":"BTCUSDT","k":{"t":1,"T":60000,"s":"BTCUSDT","i":"1m","o":"1","h":"2","l":"0.5","c":"1.5","v":"0.00000001","x":true}})";
    const auto parsed = binance::parse_kline(one_atom);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->volume, 1);
}

TEST(BinanceParser, ClosedKlineRejectsMalformedOrDuplicateEconomicFields)
{
    EXPECT_FALSE(binance::parse_kline(
        R"({"e":"kline","E":61000,"s":"BTCUSDT","k":{"t":1000,"T":60999,"s":"BTCUSDT","i":"1m","o":"1","h":"2","l":"0.5","c":"1.5","v":"1","v":"2","x":true}})")
                     .has_value());
    EXPECT_FALSE(binance::parse_kline(
        R"({"e":"kline","E":61000,"s":"BTCUSDT","k":{"t":1000,"T":60999,"s":"BTCUSDT","i":"1m","o":"1" "h":"2","l":"0.5","c":"1.5","v":"1","x":true}})")
                     .has_value());
    EXPECT_FALSE(binance::parse_kline(
        R"({"e":"kline","E":61000,"s":"BTCUSDT","k":{"t":1000,"T":60999,"s":"BTCUSDT","i":"1m","o":"1","h":"2","l":"0.5","c":"1.5","v":"1","x":true,}})")
                     .has_value());
}

// --- IDataParser adapter tests ---

TEST(BinanceParser, TradeParserAdapter)
{
    BinanceTradeParser parser;
    EXPECT_TRUE(parser.parse_header(""));  // no header needed

    std::string json = R"({"e":"trade","E":1001,"s":"BTCUSDT","t":1,"p":"100.0","q":"5","T":1000,"m":false})";
    auto result = parser.parse_record(json);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol, "BTCUSDT");
}

TEST(BinanceParser, KlineParserAdapter)
{
    BinanceKlineParser parser;
    EXPECT_TRUE(parser.parse_header(""));

    std::string json = R"({"e":"kline","E":61000,"s":"ETHUSDT","k":{"t":1000,"T":60999,"s":"ETHUSDT","i":"1m","o":"100","c":"101","h":"102","l":"99","v":"50","x":true}})";
    auto result = parser.parse_record(json);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol, "ETHUSDT");
    EXPECT_DOUBLE_EQ(result->close, 101.0);
}

TEST(BinanceReplayTransport, RejectsMalformedOrRegressingTimestampPrefix)
{
    const auto suffix_path = std::filesystem::temp_directory_path()
        / "truetest_binance_replay_suffix.txt";
    {
        std::ofstream out(suffix_path);
        ASSERT_TRUE(out.good());
        out << "123junk\t{}\n";
    }
    ReplayTransport suffix(suffix_path.string());
    ASSERT_TRUE(suffix.open());
    EXPECT_FALSE(suffix.read_line().has_value());
    EXPECT_EQ(suffix.terminal_status(), transport_terminal_status::failed);
    suffix.close();

    const auto regression_path = std::filesystem::temp_directory_path()
        / "truetest_binance_replay_regression.txt";
    {
        std::ofstream out(regression_path);
        ASSERT_TRUE(out.good());
        out << "200\t{\"first\":true}\n100\t{\"second\":true}\n";
    }
    ReplayTransport regression(regression_path.string());
    ASSERT_TRUE(regression.open());
    EXPECT_EQ(regression.read_line(),
              std::optional<std::string>{"{\"first\":true}"});
    EXPECT_FALSE(regression.read_line().has_value());
    EXPECT_EQ(regression.terminal_status(), transport_terminal_status::failed);
    regression.close();

    std::error_code ignored;
    std::filesystem::remove(suffix_path, ignored);
    std::filesystem::remove(regression_path, ignored);
}
