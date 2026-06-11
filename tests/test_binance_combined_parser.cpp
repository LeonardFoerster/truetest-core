// Covers the three frame shapes the combined parser must handle:
//  - raw single-stream JSON with an "e" event type  (existing path)
//  - combined-stream envelope {"stream":...,"data":{...}}  (existing path)
//  - partial-book depth frames - no "e", no "s", symbol derived from the
//    stream name  (new path added by depth integration)

#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_combined_parser.h"

#include <string>
#include <variant>

namespace {

BinanceCombinedParser make_parser() { return {}; }

}

TEST(BinanceCombinedParser, RawTradeFrame_ProducesTick)
{
    auto p = make_parser();
    const std::string frame =
        R"({"e":"trade","E":1704067200000,"s":"BTCUSDT","t":1,)"
        R"("p":"42000.5","q":"0.01","T":1704067200001,"m":false})";

    auto ev = p.parse_record(frame);
    ASSERT_TRUE(ev.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::tick>(*ev));

    const auto& t = std::get<provider::tick>(*ev);
    EXPECT_EQ(t.symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(t.price, 42000.5);
}

TEST(BinanceCombinedParser, CombinedEnvelope_TradeFrame_ProducesTick)
{
    auto p = make_parser();
    const std::string frame =
        R"({"stream":"btcusdt@trade","data":)"
        R"({"e":"trade","E":1704067200000,"s":"BTCUSDT","t":1,)"
        R"("p":"42000.5","q":"0.01","T":1704067200001,"m":false}})";

    auto ev = p.parse_record(frame);
    ASSERT_TRUE(ev.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::tick>(*ev));
}

TEST(BinanceCombinedParser, CombinedEnvelope_KlineFrame_ProducesBar)
{
    auto p = make_parser();
    const std::string frame =
        R"({"stream":"btcusdt@kline_1m","data":)"
        R"({"e":"kline","E":1704067200000,"s":"BTCUSDT","k":)"
        R"({"t":1704067200000,"T":1704067259999,"s":"BTCUSDT","i":"1m",)"
        R"("o":"42000","c":"42100","h":"42200","l":"41900","v":"5.0","x":true}}})";

    auto ev = p.parse_record(frame);
    ASSERT_TRUE(ev.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::bar>(*ev));

    const auto& b = std::get<provider::bar>(*ev);
    EXPECT_EQ(b.symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(b.close, 42100.0);
}

TEST(BinanceCombinedParser, PartialBookDepth_ProducesL2Snapshot)
{
    // @depth20@100ms format has no "e" event-type and no "s" symbol -
    // the parser must recognize it by the stream name and extract the
    // symbol from there.
    auto p = make_parser();
    const std::string frame =
        R"({"stream":"btcusdt@depth20@100ms","data":)"
        R"({"lastUpdateId":12345,"bids":[["42000.0","1.5"],["41999.5","2.0"]],)"
        R"("asks":[["42001.0","0.8"],["42002.0","1.2"]]}})";

    auto ev = p.parse_record(frame);
    ASSERT_TRUE(ev.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::l2_snapshot>(*ev));

    const auto& snap = std::get<provider::l2_snapshot>(*ev);
    EXPECT_EQ(snap.symbol, "BTCUSDT");
    ASSERT_EQ(snap.bids.size(), 2u);
    ASSERT_EQ(snap.asks.size(), 2u);
    EXPECT_DOUBLE_EQ(snap.bids[0].price, 42000.0);
    EXPECT_DOUBLE_EQ(snap.asks[0].price, 42001.0);
}

TEST(BinanceCombinedParser, DepthUpdateFrame_ProducesL2Snapshot)
{
    // Diff-stream (@depth@100ms) frames have "e":"depthUpdate". Parser
    // should still route them to an l2_snapshot (the engine's apply_l2
    // path layers the update into the registry).
    auto p = make_parser();
    const std::string frame =
        R"({"e":"depthUpdate","E":1704067200000,"s":"BTCUSDT",)"
        R"("U":1,"u":10,"b":[["42000.0","1.5"]],"a":[["42001.0","0.8"]]})";

    auto ev = p.parse_record(frame);
    ASSERT_TRUE(ev.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::l2_snapshot>(*ev));
}

TEST(BinanceCombinedParser, UnknownFrame_ReturnsNullopt)
{
    auto p = make_parser();
    const std::string frame =
        R"({"stream":"btcusdt@aggTrade","data":{"e":"aggTrade","s":"BTCUSDT"}})";

    auto ev = p.parse_record(frame);
    EXPECT_FALSE(ev.has_value());
}

TEST(BinanceCombinedParser, PartialBookWithMixedCaseStream_UppercasesSymbol)
{
    auto p = make_parser();
    const std::string frame =
        R"({"stream":"ethusdt@depth5@100ms","data":)"
        R"({"lastUpdateId":1,"bids":[["2000.0","1.0"]],"asks":[["2001.0","1.0"]]}})";

    auto ev = p.parse_record(frame);
    ASSERT_TRUE(ev.has_value());
    const auto& snap = std::get<provider::l2_snapshot>(*ev);
    EXPECT_EQ(snap.symbol, "ETHUSDT");
}

#endif // HAS_BINANCE
