#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_parser.h"

#include <chrono>
#include <string>

// --- Needle extractors ---

TEST(BybitParser, ExtractString)
{
    const std::string json =
        R"({"topic":"publicTrade.BTCUSDT","p":"97000.5"})";
    EXPECT_EQ(bybit::extract_string(json, "topic"), "publicTrade.BTCUSDT");
    EXPECT_EQ(bybit::extract_string(json, "p"), "97000.5");
    EXPECT_EQ(bybit::extract_string(json, "missing"), "");
}

TEST(BybitParser, ExtractNumberQuotedAndBare)
{
    const std::string json = R"({"T":"1710000000000","ts":1710000000001,"n":100})";
    EXPECT_EQ(bybit::extract_number(json, "T"), "1710000000000");
    EXPECT_EQ(bybit::extract_number(json, "ts"), "1710000000001");
    EXPECT_EQ(bybit::extract_number(json, "n"), "100");
}

TEST(BybitParser, ExtractBoolRequiresExactTrue)
{
    EXPECT_TRUE(bybit::extract_sv_bool(R"({"x":true})", "x"));
    EXPECT_FALSE(bybit::extract_sv_bool(R"({"x":false})", "x"));
    EXPECT_FALSE(bybit::extract_sv_bool(R"({"y":true})", "x"));
}

// --- Trade fixtures (Bybit V5 publicTrade) ---

namespace {

constexpr const char* kTradeBuy = R"({
  "topic": "publicTrade.BTCUSDT",
  "type": "snapshot",
  "ts": 1672304486868,
  "data": [
    {
      "T": 1672304486865,
      "s": "BTCUSDT",
      "S": "Buy",
      "v": "0.001",
      "p": "16578.50",
      "i": "20f43950-72e8-4e49-aa22-0d2a2d5c0c6a",
      "BT": false
    }
  ]
})";

constexpr const char* kTradeSell = R"({
  "topic": "publicTrade.ETHUSDT",
  "type": "snapshot",
  "ts": 1710000000501,
  "data": [
    {
      "T": 1710000000500,
      "s": "ETHUSDT",
      "S": "Sell",
      "v": "1.5",
      "p": "3500.25",
      "i": "2"
    }
  ]
})";

constexpr const char* kTradeMulti = R"({
  "topic": "publicTrade.BTCUSDT",
  "type": "snapshot",
  "ts": 1002,
  "data": [
    {"T":1000,"s":"BTCUSDT","S":"Buy","v":"0.1","p":"100.0","i":"1"},
    {"T":1001,"s":"BTCUSDT","S":"Sell","v":"0.2","p":"101.0","i":"2"}
  ]
})";

} // namespace

TEST(BybitParser, ParseTradeFixture_FieldsExact)
{
    auto result = bybit::parse_trade(kTradeBuy);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(result->price, 16578.50);
    EXPECT_EQ(result->quantity, static_cast<int64_t>(0.001 * 1e8));
    // Buy-taker → bid aggressor (side 0) — Binance/Bitget convention.
    EXPECT_EQ(result->side, 0);

    auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     result->timestamp.time_since_epoch())
                     .count();
    EXPECT_EQ(ts_ms, 1672304486865LL);
}

TEST(BybitParser, ParseTrade_BuyVsSellSideMapping)
{
    auto buy = bybit::parse_trade(kTradeBuy);
    ASSERT_TRUE(buy.has_value());
    EXPECT_EQ(buy->side, 0);
    EXPECT_EQ(static_cast<data_tick_side>(buy->side), data_tick_side::bid);

    auto sell = bybit::parse_trade(kTradeSell);
    ASSERT_TRUE(sell.has_value());
    EXPECT_EQ(sell->symbol, "ETHUSDT");
    EXPECT_DOUBLE_EQ(sell->price, 3500.25);
    EXPECT_EQ(sell->quantity, static_cast<int64_t>(1.5 * 1e8));
    EXPECT_EQ(sell->side, 1);
    EXPECT_EQ(static_cast<data_tick_side>(sell->side), data_tick_side::ask);
}

TEST(BybitParser, ParseAllTrades_MultiElementData)
{
    auto all = bybit::parse_all_trades(kTradeMulti);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_DOUBLE_EQ(all[0].price, 100.0);
    EXPECT_EQ(all[0].side, 0);
    EXPECT_DOUBLE_EQ(all[1].price, 101.0);
    EXPECT_EQ(all[1].side, 1);
}

TEST(BybitParser, ParseTrade_FirstOnly_ParseAllReturnsN)
{
    auto first = bybit::parse_trade(kTradeMulti);
    ASSERT_TRUE(first.has_value());
    EXPECT_DOUBLE_EQ(first->price, 100.0);

    auto all = bybit::parse_all_trades(kTradeMulti);
    ASSERT_EQ(all.size(), 2u);
}

TEST(BybitParser, ParseTrade_RejectsOrderbookTopic)
{
    constexpr const char* kOb = R"({
      "topic":"orderbook.50.BTCUSDT",
      "type":"snapshot",
      "data":{"s":"BTCUSDT","b":[["1","1"]],"a":[["2","2"]]}
    })";
    EXPECT_FALSE(bybit::parse_trade(kOb).has_value());
}

// --- Kline ---

namespace {

constexpr const char* kKlineClosed = R"({
  "topic": "kline.1.BTCUSDT",
  "type": "snapshot",
  "ts": 1672325384,
  "data": [{
    "start": 1672324800000,
    "end": 1672324859999,
    "interval": "1",
    "open": "16649.5",
    "close": "16649.5",
    "high": "16649.5",
    "low": "16649.5",
    "volume": "0.001",
    "turnover": "16.6495",
    "confirm": true,
    "timestamp": 1672325383919
  }]
})";

constexpr const char* kKlineOpen = R"({
  "topic": "kline.1.BTCUSDT",
  "type": "snapshot",
  "ts": 1672325384,
  "data": [{
    "start": 1672324800000,
    "open": "100",
    "close": "101",
    "high": "102",
    "low": "99",
    "volume": "1.0",
    "confirm": false
  }]
})";

} // namespace

TEST(BybitParser, ParseKline_ClosedFields)
{
    auto b = bybit::parse_kline(kKlineClosed);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(b->open, 16649.5);
    EXPECT_DOUBLE_EQ(b->close, 16649.5);
    EXPECT_EQ(b->date, "1672324800000");
    EXPECT_EQ(b->volume, static_cast<int64_t>(0.001 * 1e8));
}

TEST(BybitParser, KlineClosedGate_ConfirmTrueEmits)
{
    bybit::kline_closed_gate gate;
    auto closed = bybit::gated_kline_bar(gate, kKlineClosed);
    ASSERT_TRUE(closed.has_value());
    EXPECT_DOUBLE_EQ(closed->open, 16649.5);
}

TEST(BybitParser, KlineClosedGate_ConfirmFalseHolds)
{
    bybit::kline_closed_gate gate;
    auto held = bybit::gated_kline_bar(gate, kKlineOpen);
    EXPECT_FALSE(held.has_value());
    ASSERT_TRUE(gate.pending().has_value());
}

TEST(BybitParser, TradeParser_ParseRecordsEmitsAll)
{
    BybitTradeParser parser;
    auto recs = parser.parse_records(kTradeMulti);
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_DOUBLE_EQ(recs[0].price, 100.0);
    EXPECT_DOUBLE_EQ(recs[1].price, 101.0);
}

TEST(BybitParser, SymbolFromTopic)
{
    EXPECT_EQ(bybit::detail::symbol_from_topic("publicTrade.BTCUSDT"), "BTCUSDT");
    EXPECT_EQ(bybit::detail::symbol_from_topic("orderbook.50.ETHUSDT"), "ETHUSDT");
    EXPECT_EQ(bybit::detail::symbol_from_topic("kline.1.BTCUSDT"), "BTCUSDT");
}

#endif // HAS_BYBIT
