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
        R"({"e":"trade","E":1704067200002,"s":"BTCUSDT","t":1,)"
        R"("p":"42000.5","q":"0.01","T":1704067200001,"m":false})";

    auto ev = p.parse_record(frame);
    ASSERT_TRUE(ev.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::tick>(*ev));

    const auto& t = std::get<provider::tick>(*ev);
    EXPECT_EQ(t.symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(t.price, 42000.5);
}

TEST(BinanceCombinedParser, RejectsCrossFrameKnownAtRegression)
{
    BinanceCombinedParser parser;
    ASSERT_TRUE(parser.parse_record(
        R"({"e":"trade","E":2,"s":"BTCUSDT","t":2,"p":"100","q":"1","T":1,"m":false})")
                    .has_value());
    EXPECT_FALSE(parser.parse_record(
        R"({"e":"trade","E":1,"s":"BTCUSDT","t":1,"p":"99","q":"1","T":1,"m":false})")
                     .has_value());
}

// --- footprint.md §2.1 enrichment: native trade id + opt-in exact decimal ---

TEST(BinanceCombinedParser, RawTradeFrame_PopulatesNativeTradeId)
{
    auto p = make_parser();
    const std::string frame =
        R"({"e":"trade","E":1704067200002,"s":"BTCUSDT","t":987654321,)"
        R"("p":"42000.5","q":"0.01","T":1704067200001,"m":false})";

    auto ev = p.parse_record(frame);
    ASSERT_TRUE(ev.has_value());
    const auto& t = std::get<provider::tick>(*ev);
    EXPECT_EQ(t.native_trade_id, 987654321ULL);
    EXPECT_FALSE(t.has_exact_decimal); // no exact tick size configured - unchanged default
}

TEST(BinanceCombinedParser, ConfiguredExactDecimalPopulatesExactFieldsWithoutTouchingDoubles)
{
    BinanceCombinedParser p;
    p.configure_exact_decimal("0.01", 8);
    const std::string frame =
        R"({"e":"trade","E":1704067200002,"s":"BTCUSDT","t":1,)"
        R"("p":"68120.50","q":"0.01230000","T":1704067200001,"m":true})";

    auto ev = p.parse_record(frame);
    ASSERT_TRUE(ev.has_value());
    const auto& t = std::get<provider::tick>(*ev);
    EXPECT_TRUE(t.has_exact_decimal);
    EXPECT_EQ(t.price_ticks, 6812050);
    EXPECT_EQ(t.base_qty_atoms, 1230000);
    // Existing double-path fields are still populated exactly as before -
    // the enrichment is additive, matching footprint.md §2.1's requirement
    // that "existing engine conversion will ignore the enrichment".
    EXPECT_DOUBLE_EQ(t.price, 68120.50);
}

TEST(BinanceCombinedParser, UnconfiguredInstanceNeverSetsHasExactDecimal)
{
    // A fresh parser without configure_exact_decimal() must behave exactly
    // as before - no cross-instance state leakage, no accidental default-on.
    auto p = make_parser();
    const std::string frame =
        R"({"e":"trade","E":1704067200002,"s":"BTCUSDT","t":1,)"
        R"("p":"68120.50","q":"0.01230000","T":1704067200001,"m":true})";
    auto ev = p.parse_record(frame);
    ASSERT_TRUE(ev.has_value());
    EXPECT_FALSE(std::get<provider::tick>(*ev).has_exact_decimal);
}

TEST(BinanceCombinedParser, PriceFormatOurStrictParserRejectsDegradesGracefully)
{
    // std::from_chars<double> (used by the pre-existing double path) accepts
    // scientific notation; our strict decimal-only parse_decimal() rejects
    // it (Binance never actually sends this shape - this proves the guard
    // fails closed rather than crashing/miscomputing if it ever did).
    BinanceCombinedParser p;
    p.configure_exact_decimal("0.01", 8);
    const std::string frame =
        R"({"e":"trade","E":1704067200002,"s":"BTCUSDT","t":1,)"
        R"("p":"1e2","q":"0.01","T":1704067200001,"m":true})";
    auto ev = p.parse_record(frame);
    ASSERT_TRUE(ev.has_value());
    const auto& t = std::get<provider::tick>(*ev);
    EXPECT_DOUBLE_EQ(t.price, 100.0); // primary double path still succeeds
    EXPECT_FALSE(t.has_exact_decimal); // exact path fails closed, not crashes
}

TEST(BinanceCombinedParser, CombinedEnvelope_TradeFrame_ProducesTick)
{
    auto p = make_parser();
    const std::string frame =
        R"({"stream":"btcusdt@trade","data":)"
        R"({"e":"trade","E":1704067200002,"s":"BTCUSDT","t":1,)"
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
        R"({"e":"kline","E":1704067260000,"s":"BTCUSDT","k":)"
        R"({"t":1704067200000,"T":1704067259999,"s":"BTCUSDT","i":"1m",)"
        R"("o":"42000","c":"42100","h":"42200","l":"41900","v":"5.0","x":true}}})";

    auto ev = p.parse_record(frame);
    ASSERT_TRUE(ev.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::bar>(*ev));

    const auto& b = std::get<provider::bar>(*ev);
    EXPECT_EQ(b.symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(b.close, 42100.0);
}

TEST(BinanceCombinedParser, FormingKlineIsClassifiedAsIgnoredNoData)
{
    auto p = make_parser();
    const std::string frame =
        R"({"stream":"btcusdt@kline_1m","data":)"
        R"({"e":"kline","E":1704067201000,"s":"BTCUSDT","k":)"
        R"({"t":1704067200000,"T":1704067259999,"s":"BTCUSDT","i":"1m","o":"42000","c":"42050",)"
        R"("h":"42100","l":"41900","v":"2.0","x":false}}})";

    EXPECT_FALSE(p.parse_record(frame).has_value());
    EXPECT_EQ(p.classify_empty_frame(frame), empty_parse_status::ignored);
}

TEST(BinanceCombinedParser, KlineIntervalMustMatchStreamAuthority)
{
    auto p = make_parser();
    const std::string wrong_interval =
        R"({"stream":"btcusdt@kline_1m","data":)"
        R"({"e":"kline","E":1704067260000,"s":"BTCUSDT","k":)"
        R"({"t":1704067200000,"T":1704067259999,"s":"BTCUSDT","i":"1h",)"
        R"("o":"42000","c":"42100","h":"42200","l":"41900","v":"5.0","x":true}}})";
    const std::string missing_interval =
        R"({"stream":"btcusdt@kline_1m","data":)"
        R"({"e":"kline","E":1704067260000,"s":"BTCUSDT","k":)"
        R"({"t":1704067200000,"T":1704067259999,"s":"BTCUSDT",)"
        R"("o":"42000","c":"42100","h":"42200","l":"41900","v":"5.0","x":true}}})";

    EXPECT_FALSE(p.parse_record(wrong_interval).has_value());
    EXPECT_FALSE(p.parse_record(missing_interval).has_value());
}

TEST(BinanceCombinedParser, MalformedFormingKlineStillFailsClosed)
{
    auto p = make_parser();
    const std::string missing_low =
        R"({"e":"kline","s":"BTCUSDT","k":)"
        R"({"t":1,"o":"1","c":"1","h":"1","x":false}})";

    EXPECT_FALSE(p.parse_record(missing_low).has_value());
    EXPECT_EQ(p.classify_empty_frame(missing_low),
              empty_parse_status::malformed);
}

TEST(BinanceCombinedParser, IncompleteFormingKlineIsMalformedNotIgnored)
{
    auto p = make_parser();
    const std::string incomplete =
        R"({"stream":"btcusdt@kline_1m","data":)"
        R"({"e":"kline","s":"BTCUSDT","k":)"
        R"({"i":"1m","o":"1","h":"1","l":"1","c":"1","x":false}}})";

    EXPECT_FALSE(p.parse_record(incomplete).has_value());
    EXPECT_EQ(p.classify_empty_frame(incomplete),
              empty_parse_status::malformed);
}

TEST(BinanceCombinedParser, FormingKlineClockBoundariesAndZeroVolume)
{
    const auto classify = [](std::int64_t event_time,
                             std::string_view volume) {
        BinanceCombinedParser parser;
        const std::string frame =
            std::string{R"({"stream":"btcusdt@kline_1m","data":{"e":"kline","E":)"}
            + std::to_string(event_time)
            + R"(,"s":"BTCUSDT","k":{"t":1000,"T":60999,"s":"BTCUSDT","i":"1m","o":"1","h":"1","l":"1","c":"1","v":")"
            + std::string{volume} + R"(","x":false}}})";
        EXPECT_FALSE(parser.parse_record(frame).has_value());
        return parser.classify_empty_frame(frame);
    };

    EXPECT_EQ(classify(1000, "0"), empty_parse_status::ignored);
    EXPECT_EQ(classify(60999, "1"), empty_parse_status::ignored);
    EXPECT_EQ(classify(999, "1"), empty_parse_status::malformed);
    EXPECT_EQ(classify(61000, "1"), empty_parse_status::malformed);
    EXPECT_EQ(classify(2000, "-1"), empty_parse_status::malformed);
}

TEST(BinanceCombinedParser, PartialBookWithoutCausalTimeFailsClosed)
{
    // Binance partial-book payloads carry no exchange event time. Until the
    // parser API carries a separately typed receive time, accepting this
    // frame would force the engine to invent chronology.
    auto p = make_parser();
    const std::string frame =
        R"({"stream":"btcusdt@depth20@100ms","data":)"
        R"({"lastUpdateId":12345,"bids":[["42000.0","1.5"],["41999.5","2.0"]],)"
        R"("asks":[["42001.0","0.8"],["42002.0","1.2"]]}})";

    EXPECT_FALSE(p.parse_record(frame).has_value());
    EXPECT_EQ(p.classify_empty_frame(frame), empty_parse_status::malformed);
}

TEST(BinanceCombinedParser, RejectsNestedAndDuplicateEnvelopeAuthority)
{
    auto p = make_parser();
    const char* nested =
        R"({"stream":"btcusdt@trade","junk":{"data":{"e":"trade","E":1,"s":"BTCUSDT","t":1,"p":"999","q":"1","T":1,"m":false}}})";
    const char* duplicate =
        R"({"stream":"btcusdt@trade","data":{"e":"trade","E":1,"s":"BTCUSDT","t":1,"p":"100","q":"1","T":1,"m":false},"data":{"e":"trade","E":1,"s":"BTCUSDT","t":2,"p":"999","q":"1","T":1,"m":false}})";
    EXPECT_FALSE(p.parse_record(nested).has_value());
    EXPECT_FALSE(p.parse_record(duplicate).has_value());
    EXPECT_EQ(p.classify_empty_frame(nested), empty_parse_status::malformed);
    EXPECT_EQ(p.classify_empty_frame(duplicate), empty_parse_status::malformed);
}

TEST(BinanceCombinedParser, RejectsWrapperPayloadChannelOrSymbolMismatch)
{
    auto p = make_parser();
    const char* wrong_symbol =
        R"({"stream":"ethusdt@trade","data":{"e":"trade","E":1,"s":"BTCUSDT","t":1,"p":"100","q":"1","T":1,"m":false}})";
    const char* wrong_channel =
        R"({"stream":"ethusdt@kline_1m","data":{"e":"trade","E":1,"s":"ETHUSDT","t":1,"p":"100","q":"1","T":1,"m":false}})";
    EXPECT_FALSE(p.parse_record(wrong_symbol).has_value());
    EXPECT_FALSE(p.parse_record(wrong_channel).has_value());
}

TEST(BinanceCombinedParser, DepthUpdateFrame_ExpandsToL2Deltas)
{
    // Diff-stream (@depth@100ms) frames must not be treated as snapshots:
    // omitted levels remain on the book. DataBridge uses parse_records(),
    // which emits each venue delta in frame order.
    auto p = make_parser();
    const std::string frame =
        R"({"e":"depthUpdate","E":1704067200000,"s":"BTCUSDT",)"
        R"("U":1,"u":10,"b":[["42000.0","1.5"]],"a":[["42001.0","0.8"]]})";

    const auto events = p.parse_records(frame);
    ASSERT_EQ(events.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<provider::l2_delta_batch>(events[0]));
    const auto& batch = std::get<provider::l2_delta_batch>(events[0]);
    ASSERT_EQ(batch.first_update_id, 1u);
    ASSERT_EQ(batch.final_update_id, 10u);
    ASSERT_EQ(batch.updates.size(), 2u);
    const auto& bid = batch.updates[0];
    const auto& ask = batch.updates[1];
    EXPECT_EQ(bid.side, 0u);
    EXPECT_EQ(ask.side, 1u);
    EXPECT_DOUBLE_EQ(bid.price, 42000.0);
    EXPECT_DOUBLE_EQ(ask.price, 42001.0);
}

TEST(BinanceCombinedParser, UnknownFrame_ReturnsNullopt)
{
    auto p = make_parser();
    const std::string frame =
        R"({"stream":"btcusdt@aggTrade","data":{"e":"aggTrade","s":"BTCUSDT"}})";

    auto ev = p.parse_record(frame);
    EXPECT_FALSE(ev.has_value());
}

TEST(BinanceCombinedParser, ControlAckRequiresAuthoritativeUniqueEnvelope)
{
    auto p = make_parser();
    EXPECT_EQ(p.classify_empty_frame(R"({"result":null,"id":1})"),
              empty_parse_status::ignored);
    EXPECT_EQ(p.classify_empty_frame(
                  R"(garbage "result":null,"id":1)"),
              empty_parse_status::malformed);
    EXPECT_EQ(p.classify_empty_frame(
                  R"({"result":null,"id":1,"id":2})"),
              empty_parse_status::malformed);
    EXPECT_EQ(p.classify_empty_frame(
                  R"({"result":null,"id":1} trailing)"),
              empty_parse_status::malformed);
    EXPECT_EQ(p.classify_empty_frame(R"({"result":null,"id":null})"),
              empty_parse_status::malformed);
    EXPECT_EQ(p.classify_empty_frame(R"({"result":null,"id":{}})"),
              empty_parse_status::malformed);
    EXPECT_EQ(p.classify_empty_frame(R"({"result":null,"id":[]})"),
              empty_parse_status::malformed);
    EXPECT_EQ(p.classify_empty_frame(R"({"result":null,"id":-1})"),
              empty_parse_status::malformed);
    EXPECT_EQ(p.classify_empty_frame(R"({"result":null,"id":"1"})"),
              empty_parse_status::malformed);
}

TEST(BinanceCombinedParser, PartialBookWithMixedCaseStream_UppercasesSymbol)
{
    auto p = make_parser();
    const std::string frame =
        R"({"stream":"ethusdt@depth5@100ms","data":)"
        R"({"E":1,"lastUpdateId":1,"bids":[["2000.0","1.0"]],"asks":[["2001.0","1.0"]]}})";

    auto ev = p.parse_record(frame);
    ASSERT_TRUE(ev.has_value());
    const auto& snap = std::get<provider::l2_snapshot>(*ev);
    EXPECT_EQ(snap.symbol, "ETHUSDT");
}

#endif // HAS_BINANCE
