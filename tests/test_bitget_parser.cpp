#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_parser.h"

#include <chrono>
#include <string>
#include <variant>

// --- Needle extractors ---

TEST(BitgetParser, ExtractString)
{
    const std::string json =
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"p":"97000.5"})";
    EXPECT_EQ(bitget::extract_string(json, "topic"), "publicTrade");
    EXPECT_EQ(bitget::extract_string(json, "symbol"), "BTCUSDT");
    EXPECT_EQ(bitget::extract_string(json, "p"), "97000.5");
    EXPECT_EQ(bitget::extract_string(json, "missing"), "");
}

TEST(BitgetParser, ExtractNumberQuotedAndBare)
{
    const std::string json = R"({"T":"1710000000000","ts":1710000000001,"n":100})";
    EXPECT_EQ(bitget::extract_number(json, "T"), "1710000000000");
    EXPECT_EQ(bitget::extract_number(json, "ts"), "1710000000001");
    EXPECT_EQ(bitget::extract_number(json, "n"), "100");
    EXPECT_EQ(bitget::extract_number(json, "missing"), "");
}

TEST(BitgetParser, ExtractBoolRequiresExactTrue)
{
    EXPECT_TRUE(bitget::extract_sv_bool(R"({"x":true})", "x"));
    EXPECT_FALSE(bitget::extract_sv_bool(R"({"x":false})", "x"));
    EXPECT_FALSE(bitget::extract_sv_bool(R"({"x":True})", "x"));
    EXPECT_FALSE(bitget::extract_sv_bool(R"({"y":true})", "x"));
}

TEST(BitgetParser, ExtractOptionalBoolDistinguishesMissing)
{
    auto t = bitget::extract_sv_optional_bool(R"({"confirm":true})", "confirm");
    ASSERT_TRUE(t.has_value());
    EXPECT_TRUE(*t);

    auto f = bitget::extract_sv_optional_bool(R"({"confirm":false})", "confirm");
    ASSERT_TRUE(f.has_value());
    EXPECT_FALSE(*f);

    EXPECT_FALSE(bitget::extract_sv_optional_bool(R"({"y":true})", "confirm").has_value());
}

// --- Trade fixtures (plan §9.1) ---

namespace {

constexpr const char* kTradeBuy = R"({
  "arg": {"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},
  "data": [{
    "i": "tradeId",
    "p": "97000.5",
    "v": "0.01",
    "S": "buy",
    "T": "1710000000000"
  }],
  "ts": 1710000000001
})";

constexpr const char* kTradeSell = R"({
  "arg": {"instType":"usdt-futures","topic":"publicTrade","symbol":"ETHUSDT"},
  "data": [{
    "i": "2",
    "p": "3500.25",
    "v": "1.5",
    "S": "sell",
    "T": "1710000000500"
  }],
  "ts": 1710000000501
})";

constexpr const char* kTradeMulti = R"({
  "arg": {"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},
  "data": [
    {"i":"1","p":"100.0","v":"0.1","S":"buy","T":"1000"},
    {"i":"2","p":"101.0","v":"0.2","S":"sell","T":"1001"}
  ],
  "ts": 1002
})";

} // namespace

TEST(BitgetParser, ParseTradeFixture_FieldsExact)
{
    auto result = bitget::parse_trade(kTradeBuy);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(result->price, 97000.5);
    EXPECT_EQ(result->quantity, static_cast<int64_t>(0.01 * 1e8));
    EXPECT_EQ(result->side, 0); // buy → bid aggressor

    auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     result->timestamp.time_since_epoch())
                     .count();
    EXPECT_EQ(ts_ms, 1710000000000LL);
}

TEST(BitgetParser, ParseTrade_BuyVsSellSideMapping)
{
    auto buy = bitget::parse_trade(kTradeBuy);
    ASSERT_TRUE(buy.has_value());
    EXPECT_EQ(buy->side, 0); // bid
    EXPECT_EQ(static_cast<data_tick_side>(buy->side), data_tick_side::bid);

    auto sell = bitget::parse_trade(kTradeSell);
    ASSERT_TRUE(sell.has_value());
    EXPECT_EQ(sell->symbol, "ETHUSDT");
    EXPECT_DOUBLE_EQ(sell->price, 3500.25);
    EXPECT_EQ(sell->quantity, static_cast<int64_t>(1.5 * 1e8));
    EXPECT_EQ(sell->side, 1); // ask
    EXPECT_EQ(static_cast<data_tick_side>(sell->side), data_tick_side::ask);
}

TEST(BitgetParser, ParseAllTrades_MultiElementData)
{
    auto all = bitget::parse_all_trades(kTradeMulti);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_DOUBLE_EQ(all[0].price, 100.0);
    EXPECT_EQ(all[0].side, 0);
    EXPECT_DOUBLE_EQ(all[1].price, 101.0);
    EXPECT_EQ(all[1].side, 1);
}

// Multi-trade data[] is fully accessible via parse_all_trades (N==3).
// parse_trade / BitgetCombinedParser only surface the first element.
TEST(BitgetParser, ParseAllTrades_ReturnsAllN_CombinedFirstOnly)
{
    constexpr const char* kThree = R"({
      "arg": {"topic":"publicTrade","symbol":"BTCUSDT"},
      "data": [
        {"i":"1","p":"10.0","v":"0.1","S":"buy","T":"100"},
        {"i":"2","p":"20.0","v":"0.2","S":"sell","T":"200"},
        {"i":"3","p":"30.0","v":"0.3","S":"buy","T":"300"}
      ],
      "ts": 301
    })";

    auto all = bitget::parse_all_trades(kThree);
    ASSERT_EQ(all.size(), 3u);
    EXPECT_DOUBLE_EQ(all[0].price, 10.0);
    EXPECT_DOUBLE_EQ(all[1].price, 20.0);
    EXPECT_DOUBLE_EQ(all[2].price, 30.0);
    EXPECT_EQ(all[0].symbol, "BTCUSDT");
    EXPECT_EQ(all[2].symbol, "BTCUSDT");
    EXPECT_EQ(all[0].side, 0);
    EXPECT_EQ(all[1].side, 1);
    EXPECT_EQ(all[2].side, 0);

    auto first = bitget::parse_trade(kThree);
    ASSERT_TRUE(first.has_value());
    EXPECT_DOUBLE_EQ(first->price, 10.0);

    BitgetCombinedParser combined;
    auto ev = combined.parse_record(std::string_view{kThree});
    ASSERT_TRUE(ev.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::tick>(*ev));
    EXPECT_DOUBLE_EQ(std::get<provider::tick>(*ev).price, 10.0);
}

TEST(BitgetParser, ParseTrade_WrongTopic)
{
    const char* json =
        R"({"arg":{"topic":"kline","symbol":"BTCUSDT"},"data":[{"p":"1","v":"1","S":"buy","T":"1"}]})";
    EXPECT_FALSE(bitget::parse_trade(json).has_value());
}

TEST(BitgetParser, ParseTrade_MissingFields)
{
    const char* missing_price =
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"data":[{"v":"0.01","S":"buy","T":"1"}]})";
    EXPECT_FALSE(bitget::parse_trade(missing_price).has_value());

    const char* missing_side =
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"data":[{"p":"1","v":"0.01","T":"1"}]})";
    EXPECT_FALSE(bitget::parse_trade(missing_side).has_value());

    const char* missing_symbol =
        R"({"arg":{"topic":"publicTrade"},"data":[{"p":"1","v":"0.01","S":"buy","T":"1"}]})";
    EXPECT_FALSE(bitget::parse_trade(missing_symbol).has_value());
}

TEST(BitgetParser, ParseTrade_MalformedNoCrash)
{
    EXPECT_FALSE(bitget::parse_trade("").has_value());
    EXPECT_FALSE(bitget::parse_trade("{").has_value());
    EXPECT_FALSE(bitget::parse_trade(R"({"arg":{"topic":"publicTrade","symbol":"X"},"data":[})").has_value());
    EXPECT_FALSE(bitget::parse_trade(R"({"data":"not-array"})").has_value());
}

// --- books5 fixtures (plan §9.2) ---

namespace {

constexpr const char* kBooks5 = R"({
  "arg": {"instType":"usdt-futures","topic":"books5","symbol":"BTCUSDT"},
  "action": "snapshot",
  "data": [{
    "a": [["97001.0","1.5"],["97002.0","2.0"]],
    "b": [["97000.0","0.5"],["96999.0","1.0"]],
    "ts": "1710000000000",
    "seq": "123"
  }],
  "ts": 1710000000001
})";

} // namespace

TEST(BitgetParser, ParseBooks5_BidAskLevels)
{
    auto snap = bitget::parse_books5(kBooks5);
    ASSERT_TRUE(snap.has_value());

    EXPECT_EQ(snap->symbol, "BTCUSDT");
    ASSERT_EQ(snap->bids.size(), 2u);
    ASSERT_EQ(snap->asks.size(), 2u);

    EXPECT_DOUBLE_EQ(snap->bids[0].price, 97000.0);
    EXPECT_EQ(snap->bids[0].quantity, static_cast<int64_t>(0.5 * 1e8));
    EXPECT_DOUBLE_EQ(snap->bids[1].price, 96999.0);
    EXPECT_EQ(snap->bids[1].quantity, static_cast<int64_t>(1.0 * 1e8));

    EXPECT_DOUBLE_EQ(snap->asks[0].price, 97001.0);
    EXPECT_EQ(snap->asks[0].quantity, static_cast<int64_t>(1.5 * 1e8));
    EXPECT_DOUBLE_EQ(snap->asks[1].price, 97002.0);
    EXPECT_EQ(snap->asks[1].quantity, static_cast<int64_t>(2.0 * 1e8));

    auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     snap->timestamp.time_since_epoch())
                     .count();
    EXPECT_EQ(ts_ms, 1710000000000LL);
}

TEST(BitgetParser, ParseBooks5_EmptyLevels)
{
    const char* empty =
        R"({"arg":{"topic":"books5","symbol":"BTCUSDT"},"data":[{"a":[],"b":[],"ts":"1"}]})";
    EXPECT_FALSE(bitget::parse_books5(empty).has_value());
}

TEST(BitgetParser, ParseBooks5_WrongTopic)
{
    const char* json =
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"data":[{"a":[["1","1"]],"b":[["1","1"]]}]})";
    EXPECT_FALSE(bitget::parse_books5(json).has_value());
}

TEST(BitgetParser, ParseBooks5_MissingSymbol)
{
    const char* no_sym =
        R"({"arg":{"topic":"books5"},"data":[{"a":[["1","1"]],"b":[["1","1"]],"ts":"1"}]})";
    EXPECT_FALSE(bitget::parse_books5(no_sym).has_value());
}

// books5/books1/books50: missing action OK; action=update rejected.
TEST(BitgetParser, ParseBooks5_ActionUpdateRejected)
{
    const char* update = R"({
      "arg": {"topic":"books5","symbol":"BTCUSDT"},
      "action": "update",
      "data": [{"a":[["97001.0","1.0"]],"b":[["97000.0","1.0"]],"ts":"1"}]
    })";
    EXPECT_FALSE(bitget::parse_books5(update).has_value());
}

TEST(BitgetParser, ParseBooks5_MissingActionAccepted)
{
    const char* no_action = R"({
      "arg": {"topic":"books5","symbol":"BTCUSDT"},
      "data": [{"a":[["97001.0","1.0"]],"b":[["97000.0","1.0"]],"ts":"1"}]
    })";
    auto snap = bitget::parse_books5(no_action);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->symbol, "BTCUSDT");
    ASSERT_EQ(snap->bids.size(), 1u);
}

// Full books channel: require action==snapshot; update → nullopt.
TEST(BitgetParser, ParseBooks_ActionUpdateRejected)
{
    const char* update = R"({
      "arg": {"topic":"books","symbol":"BTCUSDT"},
      "action": "update",
      "data": [{"a":[["1","1"]],"b":[["1","1"]],"ts":"1"}]
    })";
    EXPECT_FALSE(bitget::parse_books5(update).has_value());
}

TEST(BitgetParser, ParseBooks_MissingActionRejected)
{
    const char* no_action = R"({
      "arg": {"topic":"books","symbol":"BTCUSDT"},
      "data": [{"a":[["1","1"]],"b":[["1","1"]],"ts":"1"}]
    })";
    EXPECT_FALSE(bitget::parse_books5(no_action).has_value());
}

TEST(BitgetParser, ParseBooks_SnapshotAccepted)
{
    const char* snap = R"({
      "arg": {"topic":"books","symbol":"ETHUSDT"},
      "action": "snapshot",
      "data": [{"a":[["2001","0.5"]],"b":[["2000","1.0"]],"ts":"9"}]
    })";
    auto out = bitget::parse_books5(snap);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->symbol, "ETHUSDT");
    ASSERT_EQ(out->asks.size(), 1u);
    EXPECT_DOUBLE_EQ(out->asks[0].price, 2001.0);
}

TEST(BitgetParser, ParseBooks5_MalformedNoCrash)
{
    EXPECT_FALSE(bitget::parse_books5("").has_value());
    EXPECT_FALSE(bitget::parse_books5(R"({"arg":{"topic":"books5"},"data":[})").has_value());
}

// --- kline fixtures (plan §9.3) ---

namespace {

constexpr const char* kKline = R"({
  "arg": {"instType":"usdt-futures","topic":"kline","symbol":"BTCUSDT","interval":"1m"},
  "data": [{
    "start": "1710000000000",
    "open": "97000",
    "high": "97100",
    "low": "96900",
    "close": "97050",
    "volume": "100.5",
    "turnover": "9750000"
  }],
  "ts": 1710000000001
})";

} // namespace

TEST(BitgetParser, ParseKline_OHLCV)
{
    auto bar = bitget::parse_kline(kKline);
    ASSERT_TRUE(bar.has_value());

    EXPECT_EQ(bar->symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(bar->open, 97000.0);
    EXPECT_DOUBLE_EQ(bar->high, 97100.0);
    EXPECT_DOUBLE_EQ(bar->low, 96900.0);
    EXPECT_DOUBLE_EQ(bar->close, 97050.0);
    EXPECT_EQ(bar->volume, static_cast<int64_t>(100.5 * 1e8));
    EXPECT_EQ(bar->date, "1710000000000");
}

TEST(BitgetParser, ParseKline_ConfirmFalseSkips)
{
    const char* open_candle = R"({
      "arg": {"topic":"kline","symbol":"BTCUSDT","interval":"1m"},
      "data": [{
        "start":"1","open":"1","high":"2","low":"0.5","close":"1.5","volume":"10",
        "confirm":false
      }]
    })";
    EXPECT_FALSE(bitget::parse_kline(open_candle).has_value());
}

TEST(BitgetParser, ParseKline_ConfirmTrueEmits)
{
    const char* closed = R"({
      "arg": {"topic":"kline","symbol":"ETHUSDT","interval":"5m"},
      "data": [{
        "start":"2","open":"10","high":"12","low":"9","close":"11","volume":"3",
        "confirm":true
      }]
    })";
    auto bar = bitget::parse_kline(closed);
    ASSERT_TRUE(bar.has_value());
    EXPECT_EQ(bar->symbol, "ETHUSDT");
    EXPECT_DOUBLE_EQ(bar->close, 11.0);
}

TEST(BitgetParser, ParseKline_MissingOHLC)
{
    const char* missing =
        R"({"arg":{"topic":"kline","symbol":"BTCUSDT"},"data":[{"open":"1","high":"2"}]})";
    EXPECT_FALSE(bitget::parse_kline(missing).has_value());
}

TEST(BitgetParser, ParseKline_MissingSymbol)
{
    const char* no_sym = R"({
      "arg": {"topic":"kline","interval":"1m"},
      "data": [{"start":"1","open":"1","high":"2","low":"0.5","close":"1.5","volume":"10"}]
    })";
    EXPECT_FALSE(bitget::parse_kline(no_sym).has_value());
}

TEST(BitgetParser, ParseKline_MalformedNoCrash)
{
    EXPECT_FALSE(bitget::parse_kline("").has_value());
    EXPECT_FALSE(bitget::parse_kline(R"({"arg":{"topic":"kline"},"data":[})").has_value());
}

// --- Combined dispatcher + IDataParser adapters ---

TEST(BitgetParser, Combined_DispatchesByTopic)
{
    BitgetCombinedParser parser;

    auto trade_ev = parser.parse_record(std::string_view{kTradeBuy});
    ASSERT_TRUE(trade_ev.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::tick>(*trade_ev));
    EXPECT_EQ(std::get<provider::tick>(*trade_ev).symbol, "BTCUSDT");

    auto books_ev = parser.parse_record(std::string_view{kBooks5});
    ASSERT_TRUE(books_ev.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::l2_snapshot>(*books_ev));
    EXPECT_EQ(std::get<provider::l2_snapshot>(*books_ev).bids.size(), 2u);

    auto kline_ev = parser.parse_record(std::string_view{kKline});
    ASSERT_TRUE(kline_ev.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::bar>(*kline_ev));
    EXPECT_DOUBLE_EQ(std::get<provider::bar>(*kline_ev).close, 97050.0);
}

TEST(BitgetParser, Combined_UnknownTopic)
{
    BitgetCombinedParser parser;
    const char* json =
        R"({"arg":{"topic":"ticker","symbol":"BTCUSDT"},"data":[{"last":"1"}]})";
    EXPECT_FALSE(parser.parse_record(std::string_view{json}).has_value());
}

TEST(BitgetParser, TradeParserAdapter)
{
    BitgetTradeParser parser;
    EXPECT_TRUE(parser.parse_header(""));
    auto rec = parser.parse_record(std::string_view{kTradeBuy});
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->symbol, "BTCUSDT");
    EXPECT_EQ(rec->side, data_tick_side::bid);
    EXPECT_EQ(rec->quantity, static_cast<int64_t>(0.01 * 1e8));
}

TEST(BitgetParser, KlineParserAdapter)
{
    BitgetKlineParser parser;
    auto rec = parser.parse_record(std::string_view{kKline});
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(rec->close, 97050.0);
}

TEST(BitgetParser, BooksParserAdapter)
{
    BitgetBooksParser parser;
    auto snap = parser.parse_record(std::string_view{kBooks5});
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->symbol, "BTCUSDT");
    EXPECT_EQ(snap->asks.size(), 2u);
}

#endif // HAS_BITGET
