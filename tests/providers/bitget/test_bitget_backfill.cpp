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

TEST(BitgetBackfill, EncodeKlineFrameConfirmTrue)
{
    bitget::backfill_bar b;
    b.open_time = 1710000000000LL;
    b.open = 1;
    b.high = 2;
    b.low = 0.5;
    b.close = 1.5;
    b.volume = 10;
    auto frame = bitget::encode_kline_frame(b, "BTCUSDT", "1m");
    EXPECT_NE(frame.find("\"topic\":\"kline\""), std::string::npos);
    EXPECT_NE(frame.find("\"symbol\":\"BTCUSDT\""), std::string::npos);
    EXPECT_NE(frame.find("\"confirm\":true"), std::string::npos);
    EXPECT_NE(frame.find("1710000000000"), std::string::npos);

    // Closed-bar gate emits confirm:true immediately.
    BitgetKlineParser parser;
    auto rec = parser.parse_record(std::string_view{frame});
    ASSERT_TRUE(rec.has_value());
    EXPECT_DOUBLE_EQ(rec->close, 1.5);
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

TEST(BitgetBackfill, ToPrependFramesSize)
{
    std::vector<bitget::backfill_bar> bars(3);
    for (int i = 0; i < 3; ++i)
    {
        bars[i].open_time = 1000 * (i + 1);
        bars[i].open = bars[i].high = bars[i].low = bars[i].close = 1.0;
    }
    auto lines = bitget::BitgetBackfill::to_prepend_frames(bars, "ETHUSDT", "5m");
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_NE(lines[0].find("ETHUSDT"), std::string::npos);
}

#endif // HAS_BITGET
