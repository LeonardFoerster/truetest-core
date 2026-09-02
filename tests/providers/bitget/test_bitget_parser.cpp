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
    "i": "1260903622036942849",
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
    EXPECT_EQ(result->quantity_scale, 100'000'000ULL);
    EXPECT_EQ(result->side, 0); // buy → bid aggressor
    EXPECT_EQ(result->native_trade_id, 1260903622036942849ULL);

    auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     result->timestamp.time_since_epoch())
                     .count();
    EXPECT_EQ(ts_ms, 1710000000001LL);
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

TEST(BitgetParser, PublicTradeRequiresUsdtProductAndKnownAction)
{
    const char* coin_futures = R"({
      "arg":{"instType":"coin-futures","topic":"publicTrade","symbol":"BTCUSD"},
      "action":"snapshot","ts":2,
      "data":[{"i":"1","p":"100","v":"250","S":"buy","T":"1"}]
    })";
    const char* garbage_action = R"({
      "arg":{"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},
      "action":"garbage","ts":2,
      "data":[{"i":"1","p":"100","v":"1","S":"buy","T":"1"}]
    })";

    EXPECT_TRUE(bitget::parse_all_trades(coin_futures).empty());
    EXPECT_TRUE(bitget::parse_all_trades(garbage_action).empty());
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

TEST(BitgetParser, TradeRequiresStableUniqueNativeIdsAndKnownAtTime)
{
    for (const char* frame : {
             R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"ts":2,"data":[{"p":"1","v":"1","S":"buy","T":"1"}]})",
             R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"ts":2,"data":[{"i":"trade-1","p":"1","v":"1","S":"buy","T":"1"}]})",
             R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"ts":2,"data":[{"i":"0","p":"1","v":"1","S":"buy","T":"1"}]})",
             R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"data":[{"i":"1","p":"1","v":"1","S":"buy","T":"1"}]})",
         })
        EXPECT_FALSE(bitget::parse_trade(frame).has_value()) << frame;

    EXPECT_TRUE(bitget::parse_all_trades(
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"ts":3,"data":[{"i":"1","p":"1","v":"1","S":"buy","T":"1"},{"i":"1","p":"2","v":"1","S":"sell","T":"2"}]})")
                    .empty());
}

TEST(BitgetParser, StatefulAdaptersRejectCrossFrameKnownAtRegression)
{
    const char* later =
        R"({"arg":{"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},"ts":2,"data":[{"i":"2","p":"100","v":"1","S":"buy","T":"1"}]})";
    const char* earlier =
        R"({"arg":{"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},"ts":1,"data":[{"i":"1","p":"99","v":"1","S":"sell","T":"1"}]})";

    BitgetTradeParser trades;
    ASSERT_EQ(trades.parse_records(later).size(), 1u);
    EXPECT_TRUE(trades.parse_records(earlier).empty());

    BitgetCombinedParser combined;
    ASSERT_EQ(combined.parse_records(later).size(), 1u);
    EXPECT_TRUE(combined.parse_records(earlier).empty());
}

// Multi-trade data[]: parse_all_trades + production parse_records emit all N.
// parse_trade / parse_record still return first only (compat).
TEST(BitgetParser, ParseAllTrades_ReturnsAllN_ProductionPathEmitsAll)
{
    constexpr const char* kThree = R"({
      "arg": {"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},
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

    BitgetTradeParser trade_parser;
    auto recs = trade_parser.parse_records(std::string_view{kThree});
    ASSERT_EQ(recs.size(), 3u);
    EXPECT_DOUBLE_EQ(recs[0].price, 10.0);
    EXPECT_DOUBLE_EQ(recs[1].price, 20.0);
    EXPECT_DOUBLE_EQ(recs[2].price, 30.0);
    EXPECT_EQ(recs[1].side, data_tick_side::ask);

    BitgetCombinedParser combined;
    auto events = combined.parse_records(std::string_view{kThree});
    ASSERT_EQ(events.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(std::holds_alternative<provider::tick>(events[i]));
        EXPECT_DOUBLE_EQ(std::get<provider::tick>(events[i]).price,
                         all[i].price);
    }
    // parse_record remains first-only for single-record callers.
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

TEST(BitgetParser, C07_RejectsMissingOrInvalidTradeTimestamp)
{
    for (const char* timestamp : {
             static_cast<const char*>(nullptr), "0", "-1", "1x",
             "9223372036854775808"})
    {
        std::string json =
            R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},)"
            R"("data":[{"p":"1","v":"0.01","S":"buy")";
        if (timestamp)
        {
            json += R"(,"T":")";
            json += timestamp;
            json += '"';
        }
        json += "}]}";
        EXPECT_FALSE(bitget::parse_trade(json).has_value())
            << (timestamp ? timestamp : "missing");
    }
}

TEST(BitgetParser, ParseTrade_MalformedNoCrash)
{
    EXPECT_FALSE(bitget::parse_trade("").has_value());
    EXPECT_FALSE(bitget::parse_trade("{").has_value());
    EXPECT_FALSE(bitget::parse_trade(R"({"arg":{"topic":"publicTrade","symbol":"X"},"data":[})").has_value());
    EXPECT_FALSE(bitget::parse_trade(R"({"data":"not-array"})").has_value());
}

TEST(BitgetParser, RejectsUnsafeTradeQuantityBeforeIntegerConversion)
{
    for (const char* qty : {"0", "-0", "-1", "1e-8", "nan", "inf",
                            "0.000000001", "0.123456789",
                            "92233720368.54775808"})
    {
        const std::string json =
            std::string(R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},)"
                        R"("data":[{"p":"100","v":")")
            + qty + R"(","S":"buy","T":"1"}]})";
        EXPECT_FALSE(bitget::parse_trade(json).has_value()) << qty;
    }
}

TEST(BitgetParser, TradeQuantityPreservesOneAtomExactly)
{
    const auto trade = bitget::parse_trade(
        R"({"arg":{"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},"data":[{"i":"1","p":"100","v":"0.00000001","S":"buy","T":"1"}],"ts":2})");
    ASSERT_TRUE(trade.has_value());
    EXPECT_EQ(trade->quantity, 1);
}

TEST(BitgetParser, MultiTradeFrameRejectsAllOnOneInvalidEconomicElement)
{
    const char* frame = R"({
      "arg":{"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},
      "data":[
        {"p":"10","v":"1","S":"buy","T":"1"},
        {"p":"11","v":"0.000000001","S":"sell","T":"2"},
        {"p":"12","v":"1","S":"buy","T":"3"}
      ]
    })";
    EXPECT_TRUE(bitget::parse_all_trades(frame).empty());
    EXPECT_FALSE(bitget::parse_trade(frame).has_value());

    BitgetTradeParser parser;
    EXPECT_TRUE(parser.parse_records(frame).empty());
}

TEST(BitgetParser, TradeRejectsDuplicateEconomicOrEnvelopeIdentity)
{
    EXPECT_FALSE(bitget::parse_trade(
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"data":[{"p":"100","v":"1","v":"2","S":"buy","T":"1"}]})")
                     .has_value());
    EXPECT_FALSE(bitget::parse_trade(
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT","symbol":"ETHUSDT"},"data":[{"p":"100","v":"1","S":"buy","T":"1"}]})")
                     .has_value());
    EXPECT_FALSE(bitget::parse_trade(
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"arg":{"topic":"publicTrade","symbol":"ETHUSDT"},"data":[{"p":"100","v":"1","S":"buy","T":"1"}]})")
                     .has_value());
    EXPECT_FALSE(bitget::parse_trade(
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"data":[{"p":"100" "v":"1","S":"buy","T":"1"}]})")
                     .has_value());
    EXPECT_FALSE(bitget::parse_trade(
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"data":[{"p":"100","v":"1","S":"buy","T":"1",}]})")
                     .has_value());
    EXPECT_FALSE(bitget::parse_trade(
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"data":"wrong-type","p":"100","v":"1","S":"buy","T":"1"})")
                     .has_value());
}

TEST(BitgetParser, TradeRejectsConflictingAliasesSymbolAndCausalOrder)
{
    EXPECT_FALSE(bitget::parse_trade(
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"data":[{"p":"100","v":"1","S":"buy","side":"sell","T":"1000","ts":"999"}]})")
                     .has_value());
    EXPECT_FALSE(bitget::parse_trade(
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"symbol":"ETHUSDT","data":[{"p":"100","v":"1","S":"buy","T":"1000"}]})")
                     .has_value());
    EXPECT_TRUE(bitget::parse_all_trades(
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"ts":1002,"data":[{"p":"100","v":"1","S":"buy","T":"1001"},{"p":"101","v":"1","S":"sell","T":"1000"}]})")
                    .empty());
    EXPECT_TRUE(bitget::parse_all_trades(
        R"({"arg":{"topic":"publicTrade","symbol":"BTCUSDT"},"ts":1002,"data":[{"p":"100","v":"1","S":"buy","T":"1003"}]})")
                    .empty());
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
    EXPECT_EQ(snap->quantity_scale, 100'000'000ULL);
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
    EXPECT_EQ(ts_ms, 1710000000001LL);
}

TEST(BitgetParser, BooksRequireCausalEnvelopeAndDataTimes)
{
    EXPECT_FALSE(bitget::parse_books5(
        R"({"arg":{"topic":"books5","symbol":"BTCUSDT"},"data":[{"a":[["2","1"]],"b":[["1","1"]],"ts":"2000"}],"ts":1000})")
                     .has_value());
    EXPECT_FALSE(bitget::parse_books5(
        R"({"arg":{"topic":"books5","symbol":"BTCUSDT"},"data":[{"a":[["2","1"]],"b":[["1","1"]],"ts":"1000"}]})")
                     .has_value());
}

TEST(BitgetParser, ParseBooks5_EmptyLevels)
{
    const char* empty =
        R"({"arg":{"topic":"books5","symbol":"BTCUSDT"},"data":[{"a":[],"b":[],"ts":"1"}]})";
    EXPECT_FALSE(bitget::parse_books5(empty).has_value());
}

TEST(BitgetParser, C07_RejectsMissingOrInvalidDepthTimestamp)
{
    for (const char* timestamp : {
             static_cast<const char*>(nullptr), "0", "-1", "1x",
             "9223372036854775808"})
    {
        std::string json =
            R"({"arg":{"topic":"books5","symbol":"BTCUSDT"},)"
            R"("action":"snapshot","data":[{"a":[["2","1"]],)"
            R"("b":[["1","1"]])";
        if (timestamp)
        {
            json += R"(,"ts":")";
            json += timestamp;
            json += '"';
        }
        json += "}]}";
        EXPECT_FALSE(bitget::parse_books5(json).has_value())
            << (timestamp ? timestamp : "missing");
    }
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
      "data": [{"a":[["97001.0","1.0"]],"b":[["97000.0","1.0"]],"ts":"1"}],
      "ts": 2
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
      "data": [{"a":[["2001","0.5"]],"b":[["2000","1.0"]],"ts":"9"}],
      "ts": 10
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

TEST(BitgetParser, BooksSnapshotRejectsZeroSubatomAndRoundedQuantity)
{
    for (const char* qty : {"0", "0.000000001", "0.123456789", "1e-8",
                            "92233720368.54775808"})
    {
        const std::string json =
            std::string{R"({"arg":{"topic":"books5","symbol":"BTCUSDT"},"data":[{"a":[["101",")"}
            + qty + R"("]],"b":[["100","1"]],"ts":"1"}]})";
        EXPECT_FALSE(bitget::parse_books5(json).has_value()) << qty;
    }
}

TEST(BitgetParser, BooksRejectMalformedLevelAndDataArrayGrammar)
{
    for (const char* levels : {
             R"([["101" "1"]])",
             R"([["101","1","ignored"]])",
             R"([["101","1"],])",
         }) {
        const std::string json =
            std::string{R"({"arg":{"topic":"books5","symbol":"BTCUSDT"},"data":[{"a":)"}
            + levels + R"(,"b":[["100","1"]],"ts":"1"}]})";
        EXPECT_FALSE(bitget::parse_books5(json).has_value()) << levels;
    }

    EXPECT_FALSE(bitget::parse_books5(
        R"({"arg":{"topic":"books5","symbol":"BTCUSDT"},"data":[{"a":[["101","1"]],"b":[["100","1"]],"ts":"1"},]})")
                     .has_value());
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
    EXPECT_EQ(bar->quantity_scale, 100'000'000ULL);
    EXPECT_EQ(bar->date, "1710000000000");
}

TEST(BitgetParser, ParseKline_ConfirmFalseStillParsesRaw)
{
    // Pure parse_kline no longer filters on confirm — closed-bar policy is
    // the stateful gate on BitgetKlineParser / BitgetCombinedParser.
    const char* open_candle = R"({
      "arg": {"topic":"kline","symbol":"BTCUSDT","interval":"1m"},
      "data": [{
        "start":"1","open":"1","high":"2","low":"0.5","close":"1.5","volume":"10",
        "confirm":false
      }]
    })";
    auto bar = bitget::parse_kline(open_candle);
    ASSERT_TRUE(bar.has_value());
    EXPECT_DOUBLE_EQ(bar->close, 1.5);
    auto conf = bitget::extract_kline_confirm(open_candle);
    ASSERT_TRUE(conf.has_value());
    EXPECT_FALSE(*conf);
}

TEST(BitgetParser, RejectsUnsafeKlineVolume)
{
    for (const char* volume : {"0", "-0", "-1", "1e-8", "nan", "inf",
                               "0.000000001", "0.123456789",
                               "92233720368.54775808"})
    {
        const std::string json =
            std::string(R"({"arg":{"topic":"kline","symbol":"BTCUSDT"},)"
                        R"("data":[{"start":"1","open":"1","high":"2",)"
                        R"("low":"0.5","close":"1.5","volume":")")
            + volume + R"("}]})";
        EXPECT_FALSE(bitget::parse_kline(json).has_value()) << volume;
    }
}

TEST(BitgetParser, KlineVolumePreservesOneAtomExactly)
{
    const auto bar = bitget::parse_kline(
        R"({"arg":{"topic":"kline","symbol":"BTCUSDT"},"data":[{"start":"1","open":"1","high":"2","low":"0.5","close":"1.5","volume":"0.00000001"}]})");
    ASSERT_TRUE(bar.has_value());
    EXPECT_EQ(bar->volume, 1);
}

TEST(BitgetParser, RejectsImpossibleKlineOhlcGeometry)
{
    for (const char* body : {
             R"("start":"1","open":"10","high":"9","low":"8","close":"9")",
             R"("start":"1","open":"10","high":"12","low":"11","close":"11")",
             R"("start":"1","open":"10","high":"8","low":"9","close":"9.5")"})
    {
        const std::string json =
            std::string{R"({"arg":{"topic":"kline","symbol":"BTCUSDT"},"data":[{)"}
            + body + R"(,"volume":"1"}]})";
        EXPECT_FALSE(bitget::parse_kline(json).has_value()) << body;
    }
}

TEST(BitgetParser, RawKlineRejectsAliasesAndMultipleDataObjects)
{
    EXPECT_FALSE(bitget::parse_kline(
        R"({"arg":{"topic":"kline","symbol":"BTCUSDT"},"data":[{"start":"1","t":"2","open":"10","o":"11","high":"12","low":"8","close":"11","volume":"1"}]})")
                     .has_value());
    EXPECT_FALSE(bitget::parse_kline(
        R"({"arg":{"topic":"kline","symbol":"BTCUSDT"},"data":[{"start":"1","open":"10","high":"12","low":"8","close":"11","volume":"1"},{"start":"2","open":"11","high":"13","low":"10","close":"12","volume":"1"}]})")
                     .has_value());
    EXPECT_FALSE(bitget::parse_books5(
        R"({"arg":{"topic":"books5","symbol":"BTCUSDT"},"data":[{"a":[["101","1"]],"b":[["100","1"]],"ts":"1"},{"a":[["102","1"]],"b":[["99","1"]],"ts":"2"}]})")
                     .has_value());
}

TEST(BitgetParser, RejectsMissingOrInvalidKlineStartTime)
{
    for (const char* start : {
             static_cast<const char*>(nullptr), "0", "-1", "1x",
             "9223372036854775807", "9223372036854775808"})
    {
        std::string body;
        if (start)
        {
            body = R"("start":")";
            body += start;
            body += R"(",)";
        }
        body += R"("open":"10","high":"12","low":"8","close":"11","volume":"1")";
        const std::string json =
            std::string{R"({"arg":{"topic":"kline","symbol":"BTCUSDT","interval":"1m"},"data":[{)"}
            + body + "}]}";
        EXPECT_FALSE(bitget::parse_kline(json).has_value())
            << (start ? start : "missing");
    }
}

TEST(BitgetParser, RejectsMissingKlineVolume)
{
    const char* json = R"({
      "arg":{"topic":"kline","symbol":"BTCUSDT","interval":"1m"},
      "data":[{"start":"1","open":"10","high":"12","low":"8","close":"11"}]
    })";
    EXPECT_FALSE(bitget::parse_kline(json).has_value());
}

TEST(BitgetParser, ParseKline_ConfirmTrueStillParsesRaw)
{
    const char* closed = R"({
      "arg": {"topic":"kline","symbol":"ETHUSDT","interval":"5m"},
      "data": [{
        "start":"2","open":"10","high":"12","low":"9","close":"11","volume":"3",
        "confirm":true
      }],
      "ts":3
    })";
    auto bar = bitget::parse_kline(closed);
    ASSERT_TRUE(bar.has_value());
    EXPECT_EQ(bar->symbol, "ETHUSDT");
    EXPECT_DOUBLE_EQ(bar->close, 11.0);
}

TEST(BitgetParser, KlineClosedGate_ConfirmFalseBuffersConfirmTrueEmits)
{
    bitget::kline_closed_gate gate;
    provider::bar open_b;
    open_b.symbol = "BTCUSDT";
    open_b.date = "1";
    open_b.open = 1;
    open_b.high = 2;
    open_b.low = 0.5;
    open_b.close = 1.5;

    EXPECT_FALSE(gate.on_bar(open_b, /*confirm=*/false).has_value());

    provider::bar closed_b = open_b;
    closed_b.close = 1.8;
    auto emitted = gate.on_bar(closed_b, /*confirm=*/true, 2);
    ASSERT_TRUE(emitted.has_value());
    EXPECT_DOUBLE_EQ(emitted->close, 1.8);
}

TEST(BitgetParser, KlineClosedGate_UtaStartRolloverEmitsPrevious)
{
    // UTA has no confirm — mid-candle updates must not emit; start change does.
    bitget::kline_closed_gate gate;
    provider::bar a;
    a.symbol = "BTCUSDT";
    a.date = "1000";
    a.open = 10;
    a.high = 11;
    a.low = 9;
    a.close = 10.5;
    a.volume = 1;

    EXPECT_FALSE(gate.on_bar(a, std::nullopt).has_value());

    provider::bar a2 = a;
    a2.high = 12;
    a2.close = 11;
    EXPECT_FALSE(gate.on_bar(a2, std::nullopt).has_value()); // same start

    provider::bar b = a;
    b.date = "2000";
    b.open = 11;
    b.high = 11.5;
    b.low = 10.5;
    b.close = 11.2;
    auto closed = gate.on_bar(b, std::nullopt);
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->date, "2000");
    EXPECT_DOUBLE_EQ(closed->close, 11.0); // last update of period 1000
    EXPECT_DOUBLE_EQ(closed->high, 12.0);
}

TEST(BitgetParser, KlineClosedGateRejectsLateAndCrossSymbolMutation)
{
    bitget::kline_closed_gate gate;
    provider::bar pending;
    pending.symbol = "BTCUSDT";
    pending.date = "2000";
    pending.open = pending.high = pending.low = pending.close = 1.0;
    ASSERT_FALSE(gate.on_bar(pending).has_value());

    provider::bar late = pending;
    late.date = "1000";
    EXPECT_FALSE(gate.on_bar(late).has_value());

    provider::bar foreign = pending;
    foreign.symbol = "ETHUSDT";
    foreign.date = "3000";
    EXPECT_FALSE(gate.on_bar(foreign).has_value());

    provider::bar next = pending;
    next.date = "3000";
    const auto emitted = gate.on_bar(next);
    ASSERT_TRUE(emitted.has_value());
    EXPECT_EQ(emitted->symbol, "BTCUSDT");
    EXPECT_EQ(emitted->date, "3000");
}

TEST(BitgetParser, KlineClosedGateRejectsMismatchedExplicitFinal)
{
    bitget::kline_closed_gate gate;
    provider::bar pending;
    pending.symbol = "BTCUSDT";
    pending.date = "1000";
    pending.open = pending.high = pending.low = pending.close = 1.0;
    ASSERT_FALSE(gate.on_bar(pending, false).has_value());

    provider::bar wrong_start = pending;
    wrong_start.date = "2000";
    EXPECT_FALSE(gate.on_bar(wrong_start, true, 2500).has_value());

    const auto finalized = gate.on_bar(pending, true, 1500);
    ASSERT_TRUE(finalized.has_value());
    EXPECT_EQ(finalized->date, "1500");
}

TEST(BitgetParser, PublicPayloadUsesOnlyTopLevelAuthority)
{
    const char* trade = R"({
      "meta":{"arg":{"topic":"publicTrade","symbol":"EVIL"},
              "data":[{"p":"999","v":"1","S":"Buy","T":"1"}]},
      "arg":{"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},
      "data":[{"i":"2","p":"100","v":"2","S":"Buy","T":"2"}],
      "ts":3
    })";
    const auto tick = bitget::parse_trade(trade);
    ASSERT_TRUE(tick.has_value());
    EXPECT_EQ(tick->symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(tick->price, 100.0);
    EXPECT_EQ(tick->quantity, 200'000'000);

    const char* books = R"({
      "meta":{"data":[{"a":[["999","1"]],"b":[["998","1"]],"ts":"1"}]},
      "arg":{"topic":"books5","symbol":"BTCUSDT"},
      "data":[{"a":[["101","1"]],"b":[["100","1"]],"ts":"2"}],
      "ts":3
    })";
    const auto snapshot = bitget::parse_books5(books);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_DOUBLE_EQ(snapshot->bids.front().price, 100.0);

    const char* kline = R"({
      "meta":{"data":[{"start":"1","open":"999","high":"999","low":"999","close":"999","volume":"1"}]},
      "arg":{"topic":"kline","symbol":"BTCUSDT"},
      "data":[{"start":"2","open":"100","high":"102","low":"99","close":"101","volume":"1"}]
    })";
    const auto bar = bitget::parse_kline(kline);
    ASSERT_TRUE(bar.has_value());
    EXPECT_DOUBLE_EQ(bar->open, 100.0);
}

TEST(BitgetParser, KlineClosedGateRejectsReplayAndLateRearm)
{
    bitget::kline_closed_gate gate;
    provider::bar a;
    a.symbol = "BTCUSDT";
    a.date = "1000";
    a.open = a.high = a.low = a.close = 1.0;

    ASSERT_TRUE(gate.on_bar(a, true, 1500).has_value());
    EXPECT_FALSE(gate.on_bar(a, true, 1500).has_value());
    EXPECT_FALSE(gate.on_bar(a, false).has_value());

    auto b = a;
    b.date = "2000";
    EXPECT_FALSE(gate.on_bar(b, false).has_value());
    EXPECT_FALSE(gate.on_bar(a, false).has_value());
}

TEST(BitgetParser, StringConfirmIsMalformedAndDoesNotMutateGate)
{
    BitgetKlineParser parser;
    const char* malformed = R"({
      "arg":{"topic":"kline","symbol":"BTCUSDT"},
      "data":[{"start":"1000","open":"1","high":"2","low":"0.5",
               "close":"1.5","volume":"1","confirm":"true"}],
      "ts":1500
    })";
    EXPECT_FALSE(parser.parse_record(std::string_view{malformed}).has_value());
    EXPECT_EQ(parser.classify_empty_frame(malformed),
              empty_parse_status::malformed);
}

TEST(BitgetParser, CombinedClassifiesEveryKlineAsUnsupportedMalformed)
{
    BitgetCombinedParser parser;
    const char* current = R"({
      "arg":{"topic":"kline","symbol":"BTCUSDT"},
      "data":[{"start":"2000","open":"1","high":"2","low":"0.5","close":"1.5","volume":"1"}]
    })";
    const char* late = R"({
      "arg":{"topic":"kline","symbol":"BTCUSDT"},
      "data":[{"start":"1000","open":"1","high":"2","low":"0.5","close":"1.5","volume":"1"}]
    })";
    EXPECT_TRUE(parser.parse_records(current).empty());
    EXPECT_EQ(parser.classify_empty_frame(current), empty_parse_status::malformed);
    EXPECT_TRUE(parser.parse_records(late).empty());
    EXPECT_EQ(parser.classify_empty_frame(late), empty_parse_status::malformed);
}

TEST(BitgetParser, CombinedRejectsConfirmedKlineWithoutKnownTime)
{
    BitgetCombinedParser parser;
    const char* missing_time = R"({
      "arg":{"topic":"kline","symbol":"BTCUSDT"},
      "data":[{"start":"1000","open":"1","high":"2","low":"0.5",
               "close":"1.5","volume":"1","confirm":true}]
    })";
    EXPECT_TRUE(parser.parse_records(missing_time).empty());
    EXPECT_EQ(parser.classify_empty_frame(missing_time),
              empty_parse_status::malformed);
}

TEST(BitgetParser, BitgetKlineParserFailsClosedUntilDualTimeContractExists)
{
    BitgetKlineParser parser;
    const char* t1 = R"({
      "arg": {"topic":"kline","symbol":"BTCUSDT","interval":"1m"},
      "data": [{"start":"1000","open":"10","high":"11","low":"9","close":"10.5","volume":"1"}]
    })";
    const char* t1b = R"({
      "arg": {"topic":"kline","symbol":"BTCUSDT","interval":"1m"},
      "data": [{"start":"1000","open":"10","high":"12","low":"9","close":"11","volume":"2"}]
    })";
    const char* t2 = R"({
      "arg": {"topic":"kline","symbol":"BTCUSDT","interval":"1m"},
      "data": [{"start":"2000","open":"11","high":"11.5","low":"10.5","close":"11.2","volume":"1"}]
    })";

    EXPECT_FALSE(parser.parse_record(std::string_view{t1}).has_value());
    EXPECT_FALSE(parser.parse_record(std::string_view{t1b}).has_value());
    EXPECT_FALSE(parser.parse_record(std::string_view{t2}).has_value());
}

TEST(BitgetParser, BitgetKlineParserConfirmedBarAlsoFailsClosed)
{
    BitgetKlineParser parser;
    const char* closed = R"({
      "arg": {"topic":"kline","symbol":"ETHUSDT","interval":"5m"},
      "data": [{
        "start":"2","open":"10","high":"12","low":"9","close":"11","volume":"3",
        "confirm":true
      }],
      "ts":3
    })";
    EXPECT_FALSE(parser.parse_record(std::string_view{closed}).has_value());
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

    // Kline streaming is fail-closed until open/known/decision time are
    // represented separately across the frozen engine boundary.
    EXPECT_FALSE(parser.parse_record(std::string_view{kKline}).has_value());
    const char* kKlineNext = R"({
      "arg": {"instType":"usdt-futures","topic":"kline","symbol":"BTCUSDT","interval":"1m"},
      "data": [{
        "start": "1710000060000",
        "open": "97050",
        "high": "97100",
        "low": "97000",
        "close": "97080",
        "volume": "10"
      }],
      "ts": 1710000060001
    })";
    EXPECT_FALSE(parser.parse_record(std::string_view{kKlineNext}).has_value());
}

TEST(BitgetParser, Combined_UnknownTopic)
{
    BitgetCombinedParser parser;
    const char* json =
        R"({"arg":{"topic":"ticker","symbol":"BTCUSDT"},"data":[{"last":"1"}]})";
    EXPECT_FALSE(parser.parse_record(std::string_view{json}).has_value());
}

TEST(BitgetParser, ControlAckRequiresAuthoritativeUniqueEnvelope)
{
    BitgetCombinedParser parser;
    EXPECT_EQ(parser.classify_empty_frame(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"order"}})"),
              empty_parse_status::ignored);
    EXPECT_EQ(parser.classify_empty_frame(
                  R"(garbage "event":"subscribe","code":"0")"),
              empty_parse_status::malformed);
    EXPECT_EQ(parser.classify_empty_frame(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"order"},"code":"0","code":"1"})"),
              empty_parse_status::malformed);
    EXPECT_EQ(parser.classify_empty_frame(
                  R"({"nested":{"event":"subscribe","arg":{"instType":"UTA","topic":"order"},"code":"0"}})"),
              empty_parse_status::malformed);
    EXPECT_EQ(parser.classify_empty_frame(
                  R"({"event":"pong"} trailing)"),
              empty_parse_status::malformed);
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
    // First UTA frame held; second start emits the closed previous bar.
    EXPECT_FALSE(parser.parse_record(std::string_view{kKline}).has_value());
    const char* next = R"({
      "arg": {"instType":"usdt-futures","topic":"kline","symbol":"BTCUSDT","interval":"1m"},
      "data": [{
        "start": "1710000060000",
        "open": "97050","high":"97100","low":"97000","close":"97080","volume":"10"
      }]
    })";
    EXPECT_FALSE(parser.parse_record(std::string_view{next}).has_value());
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
