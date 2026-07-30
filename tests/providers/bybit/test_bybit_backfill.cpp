#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_backfill.h"
#include "providers/bybit/bybit_combined_parser.h"

#include <string>
#include <variant>

namespace {

// Newest-first list (Bybit REST default).
constexpr const char* kKlineBody = R"({
  "retCode": 0,
  "retMsg": "OK",
  "result": {
    "symbol": "BTCUSDT",
    "category": "linear",
    "list": [
      ["1670601660000","17055.5","17060","17050","17058","100","1.0"],
      ["1670601600000","17071","17073","17027","17055.5","268611","15.7"]
    ]
  }
})";

constexpr const char* kBadRet = R"({
  "retCode": 10001,
  "retMsg": "fail",
  "result": {"list": [["1","1","1","1","1","1"]]}
})";

} // namespace

TEST(BybitBackfill, ParseKlineResponse_OrdersOldestFirst)
{
    auto bars = bybit::parse_kline_response(kKlineBody);
    ASSERT_EQ(bars.size(), 2u);
    // Reversed from venue newest-first.
    EXPECT_EQ(bars[0].open_time, 1670601600000LL);
    EXPECT_DOUBLE_EQ(bars[0].open, 17071.0);
    EXPECT_DOUBLE_EQ(bars[0].close, 17055.5);
    EXPECT_EQ(bars[1].open_time, 1670601660000LL);
    EXPECT_DOUBLE_EQ(bars[1].close, 17058.0);
}

TEST(BybitBackfill, ParseKlineResponse_RejectsNonZeroRetCode)
{
    auto bars = bybit::parse_kline_response(kBadRet);
    EXPECT_TRUE(bars.empty());
}

TEST(BybitBackfill, BuildKlineQuery)
{
    const auto q = bybit::build_kline_query("BTCUSDT", "1", 200, 0);
    EXPECT_EQ(q, "category=linear&interval=1&limit=200&symbol=BTCUSDT");

    const auto q2 = bybit::build_kline_query("ETHUSDT", "60", 50, 99);
    EXPECT_EQ(
        q2,
        "category=linear&interval=60&limit=50&symbol=ETHUSDT&end=99");
}

TEST(BybitBackfill, EncodeFrame_AcceptedByCombinedParser)
{
    bybit::backfill_bar b;
    b.open_time = 1670601600000LL;
    b.open = 10;
    b.high = 12;
    b.low = 9;
    b.close = 11;
    b.volume = 5;

    const auto frame = bybit::encode_kline_frame(b, "BTCUSDT", "1");
    BybitCombinedParser p;
    auto batch = p.parse_records(frame);
    ASSERT_EQ(batch.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<provider::bar>(batch[0]));
    auto& bar = std::get<provider::bar>(batch[0]);
    EXPECT_EQ(bar.symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(bar.close, 11.0);
    EXPECT_EQ(bar.date, "1670601600000");
}

TEST(BybitBackfill, ToPrependFrames_Count)
{
    std::vector<bybit::backfill_bar> bars(2);
    bars[0] = {1000, 1, 2, 0.5, 1.5, 10};
    bars[1] = {2000, 1.5, 2.5, 1, 2, 11};
    auto frames = bybit::BybitBackfill::to_prepend_frames(
        bars, "BTCUSDT", "1m");
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_NE(frames[0].find("kline.1.BTCUSDT"), std::string::npos);
    EXPECT_NE(frames[0].find("\"confirm\":true"), std::string::npos);
}

#endif // HAS_BYBIT
