#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_backfill.h"

TEST(BitgetBackfill, ParseCandlesResponseOldestFirst)
{
    // Newest-first venue payload → reverse to chronological.
    const char* body = R"({
      "code":"00000","msg":"success",
      "data":[
        ["2000","11","12","10","11.5","2","100"],
        ["1000","10","11","9","10.5","1","50"]
      ]
    })";
    auto bars = bitget::parse_candles_response(body);
    ASSERT_EQ(bars.size(), 2u);
    EXPECT_EQ(bars[0].open_time, 1000);
    EXPECT_DOUBLE_EQ(bars[0].open, 10.0);
    EXPECT_DOUBLE_EQ(bars[0].close, 10.5);
    EXPECT_EQ(bars[1].open_time, 2000);
    EXPECT_DOUBLE_EQ(bars[1].high, 12.0);
}

TEST(BitgetBackfill, ParseBusinessFailEmpty)
{
    auto bars = bitget::parse_candles_response(
        R"({"code":"40001","msg":"fail","data":[]})");
    EXPECT_TRUE(bars.empty());
}

TEST(BitgetBackfill, RejectsWholeResponseWhenAnyRowIsMalformed)
{
    const char* body = R"({
      "code":"00000","msg":"success",
      "data":[
        ["1710000060000","11","12","10","11.5","2","100"],
        ["1710000000000","10","11","9","10.5","nan","50"]
      ]
    })";
    EXPECT_TRUE(bitget::parse_candles_response(body).empty());
}

TEST(BitgetBackfill, RequiresUniqueAuthoritativeTopLevelDataArray)
{
    const char* nested_only = R"({
      "code":"00000","msg":"success",
      "meta":{"data":[
        ["1710000000000","10","11","9","10.5","1","50"]
      ]}
    })";
    EXPECT_TRUE(bitget::parse_candles_response(nested_only).empty());

    const char* nested_before_malformed_top_level = R"({
      "code":"00000","msg":"success",
      "meta":{"data":[
        ["1710000000000","10","11","9","10.5","1","50"]
      ]},
      "data":[["malformed"]]
    })";
    EXPECT_TRUE(
        bitget::parse_candles_response(nested_before_malformed_top_level)
            .empty());

    const char* duplicate_top_level = R"({
      "code":"00000",
      "data":[],
      "data":[
        ["1710000000000","10","11","9","10.5","1","50"]
      ]
    })";
    EXPECT_TRUE(bitget::parse_candles_response(duplicate_top_level).empty());

    EXPECT_TRUE(bitget::parse_candles_response(
        R"({"code":"00000","data":{"not":"an array"}})")
                    .empty());
}

TEST(BitgetBackfill, RejectsInvalidFinancialRows)
{
    for (const char* row : {
             R"(["0","10","11","9","10.5","1","50"])",
             R"(["1710000000000","-10","11","9","10.5","1","50"])",
             R"(["1710000000000","10","9","8","10.5","1","50"])",
             R"(["1710000000000","10","11","10.75","10.5","1","50"])",
             R"(["1710000000000","10","11","9","10.5","-1","50"])",
             R"(["1710000000000","10","11","9","10.5","0.000000001","50"])",
             R"(["1710000000000","10","11","9","10.5"])",
             R"(["1710000000000","10","11","9","10.5","1","inf"])",
             "[\"1710000000000\",\"10\",\"11\",\"9\",\"10.5\",\"1\",\"50\",\"extra\"]"})
    {
        const std::string body =
            std::string{R"({"code":"00000","msg":"success","data":[)"}
            + row + "]}";
        EXPECT_TRUE(bitget::parse_candles_response(body).empty()) << row;
    }
}

TEST(BitgetBackfill, RejectsDuplicateOrNonMonotoneRows)
{
    const char* duplicate = R"({
      "code":"00000","msg":"success",
      "data":[
        ["1710000000000","10","11","9","10.5","1","50"],
        ["1710000000000","10","11","9","10.5","1","50"]
      ]
    })";
    EXPECT_TRUE(bitget::parse_candles_response(duplicate).empty());

    const char* non_monotone = R"({
      "code":"00000","msg":"success",
      "data":[
        ["1710000120000","12","13","11","12.5","1","50"],
        ["1710000000000","10","11","9","10.5","1","50"],
        ["1710000060000","11","12","10","11.5","1","50"]
      ]
    })";
    EXPECT_TRUE(bitget::parse_candles_response(non_monotone).empty());
}

TEST(BitgetBackfill, PrependFramesAreUnsupportedWithoutWarmupBarrier)
{
    bitget::backfill_bar b;
    b.open_time = 1710000000000LL;
    b.open = 1;
    b.high = 2;
    b.low = 0.5;
    b.close = 1.5;
    b.volume = 10;

    EXPECT_THROW(
        (void)bitget::BitgetBackfill::to_prepend_frames(
            {b}, "BTCUSDT", "1m"),
        std::logic_error);
}

TEST(BitgetBackfill, FetchIsExplicitlyUnsupportedBeforeNetworkIo)
{
    bitget::BitgetBackfill backfill("must-not-be-contacted.invalid");
    EXPECT_THROW(
        (void)backfill.fetch("BTCUSDT", "1m", 1),
        std::logic_error);
    EXPECT_TRUE(backfill.fetch("BTCUSDT", "1m", 0).empty());
}

TEST(BitgetBackfill, CandlesQueryAlphabetical)
{
    auto q = bitget::candles_query("USDT-FUTURES", "BTCUSDT", "1m", 50, 0);
    EXPECT_EQ(q.find("category="), 0u);
    EXPECT_NE(q.find("&interval=1m"), std::string::npos);
    EXPECT_NE(q.find("&symbol=BTCUSDT"), std::string::npos);
    // category < interval < limit < symbol
    EXPECT_LT(q.find("category="), q.find("interval="));
    EXPECT_LT(q.find("interval="), q.find("limit="));
    EXPECT_LT(q.find("limit="), q.find("symbol="));
}

TEST(BitgetBackfill, EmptyPrependRemainsANoOp)
{
    EXPECT_TRUE(bitget::BitgetBackfill::to_prepend_frames(
                    {}, "ETHUSDT", "5m")
                    .empty());
}

#endif // HAS_BITGET
