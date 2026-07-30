#include <gtest/gtest.h>

#ifdef HAS_GATE

#include "providers/gate/gate_combined_parser.h"
#include "providers/gate/gate_depth_sync.h"
#include "providers/gate/gate_backfill.h"

#include <fstream>
#include <string>
#include <variant>

namespace {

std::string load_fixture(const char* name)
{
    // Try relative paths used by ctest from build dir and repo root.
    const char* candidates[] = {
        "tests/fixtures/gate/",
        "../tests/fixtures/gate/",
        "../../tests/fixtures/gate/",
        "../../../tests/fixtures/gate/",
        "/home/leonard/work/projects/truetest/core-provider-gate/tests/fixtures/gate/",
    };
    for (const char* base : candidates)
    {
        std::string path = std::string(base) + name;
        std::ifstream in(path);
        if (!in) continue;
        return std::string(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
    }
    return {};
}

} // namespace

TEST(GateCombinedParser, TradesEmitTwoPublicFilterInternal)
{
    auto json = load_fixture("trades_update.json");
    ASSERT_FALSE(json.empty()) << "fixture trades_update.json not found";

    GateCombinedParser parser;
    auto events = parser.parse_records(json);
    ASSERT_EQ(events.size(), 2u);

    ASSERT_TRUE(std::holds_alternative<provider::tick>(events[0]));
    auto t0 = std::get<provider::tick>(events[0]);
    EXPECT_EQ(t0.symbol, "BTC_USDT");
    EXPECT_DOUBLE_EQ(t0.price, 65000.1);
    EXPECT_EQ(t0.side, 1); // sell (size < 0)
    EXPECT_EQ(t0.quantity, static_cast<int64_t>(3 * 1e8));

    ASSERT_TRUE(std::holds_alternative<provider::tick>(events[1]));
    auto t1 = std::get<provider::tick>(events[1]);
    EXPECT_EQ(t1.side, 0); // buy
    EXPECT_EQ(t1.quantity, static_cast<int64_t>(2 * 1e8));
}

TEST(GateCombinedParser, IncrementalBookEmitsL2Updates)
{
    auto json = load_fixture("order_book_update.json");
    ASSERT_FALSE(json.empty());

    GateCombinedParser parser;
    auto events = parser.parse_records(json);
    ASSERT_GE(events.size(), 2u);

    // All should be l2_update for full:false.
    for (const auto& ev : events)
        EXPECT_TRUE(std::holds_alternative<provider::l2_update>(ev));

    auto u0 = std::get<provider::l2_update>(events[0]);
    EXPECT_EQ(u0.symbol, "BTC_USDT");
    EXPECT_DOUBLE_EQ(u0.price, 54672.1);
    EXPECT_EQ(u0.new_quantity, 0); // delete
    EXPECT_EQ(u0.side, 0);         // bid
}

TEST(GateCombinedParser, FullBookEmitsSnapshot)
{
    auto json = load_fixture("order_book_full.json");
    ASSERT_FALSE(json.empty());

    GateCombinedParser parser;
    auto events = parser.parse_records(json);
    ASSERT_EQ(events.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<provider::l2_snapshot>(events[0]));
    auto snap = std::get<provider::l2_snapshot>(events[0]);
    EXPECT_EQ(snap.symbol, "BTC_USDT");
    EXPECT_EQ(snap.bids.size(), 2u);
    EXPECT_EQ(snap.asks.size(), 2u);
    EXPECT_DOUBLE_EQ(snap.bids[0].price, 54672.1);
    EXPECT_EQ(snap.bids[0].quantity, static_cast<int64_t>(10 * 1e8));
}

TEST(GateCombinedParser, SubscribeAckDropped)
{
    constexpr auto ack =
        R"({"time":1,"channel":"futures.trades","event":"subscribe","result":{"status":"success"}})";
    GateCombinedParser parser;
    auto events = parser.parse_records(ack);
    EXPECT_TRUE(events.empty());
}

TEST(GateCombinedParser, CandlestickFromEncodedBackfill)
{
    gate::backfill_bar b;
    b.open_time = 1710000000;
    b.open = 100;
    b.high = 110;
    b.low = 90;
    b.close = 105;
    b.volume = 50;
    auto frame = gate::encode_candlestick_frame(b, "BTC_USDT", "1m");

    GateCombinedParser parser;
    auto events = parser.parse_records(frame);
    ASSERT_EQ(events.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<provider::bar>(events[0]));
    auto bar = std::get<provider::bar>(events[0]);
    EXPECT_EQ(bar.symbol, "BTC_USDT");
    EXPECT_DOUBLE_EQ(bar.open, 100);
    EXPECT_DOUBLE_EQ(bar.close, 105);
}

TEST(GateDepthSync, BridgeAndGapPolicy)
{
    gate::depth_sync_state st;
    gate::reset_depth_sync(st, /*base_id=*/100);

    // Before bridge: update entirely after base without covering base+1.
    EXPECT_EQ(gate::classify_book_update(st, 105, 110),
              gate::depth_action::buffer);

    // Stale before base.
    EXPECT_EQ(gate::classify_book_update(st, 90, 99),
              gate::depth_action::drop);

    // Bridge: U <= 101 <= u
    EXPECT_EQ(gate::classify_book_update(st, 100, 103),
              gate::depth_action::apply);
    gate::apply_book_update(st, 100, 103);
    EXPECT_TRUE(st.synced);
    EXPECT_EQ(st.last_u, 103);

    // Contiguous.
    EXPECT_EQ(gate::classify_book_update(st, 104, 106),
              gate::depth_action::apply);
    gate::apply_book_update(st, 104, 106);

    // Duplicate.
    EXPECT_EQ(gate::classify_book_update(st, 100, 106),
              gate::depth_action::drop);

    // Gap.
    EXPECT_EQ(gate::classify_book_update(st, 110, 112),
              gate::depth_action::resync);

    // full always applies.
    EXPECT_EQ(gate::classify_book_update(st, 999, 999, /*full=*/true),
              gate::depth_action::apply);
}

TEST(GateDepthSync, ParseRestOrderBook)
{
    auto json = load_fixture("rest_order_book.json");
    ASSERT_FALSE(json.empty());

    auto snap = gate::parse_rest_order_book(json, "BTC_USDT");
    ASSERT_TRUE(snap.ok);
    EXPECT_EQ(snap.id, 2517661100);
    EXPECT_EQ(snap.book.symbol, "BTC_USDT");
    EXPECT_EQ(snap.book.bids.size(), 2u);
    EXPECT_EQ(snap.book.asks.size(), 2u);
    EXPECT_DOUBLE_EQ(snap.book.asks[0].price, 54743.6);
}

TEST(GateBackfill, ParseCandlesticksResponse)
{
    auto json = load_fixture("candlesticks.json");
    ASSERT_FALSE(json.empty());

    auto bars = gate::parse_candlesticks_response(json);
    ASSERT_EQ(bars.size(), 2u);
    EXPECT_EQ(bars[0].open_time, 1710000000);
    EXPECT_DOUBLE_EQ(bars[0].open, 64950.5);
    EXPECT_DOUBLE_EQ(bars[1].close, 65010.0);
    // chronological
    EXPECT_LT(bars[0].open_time, bars[1].open_time);
}

TEST(GateBackfill, PrependFramesRoundTrip)
{
    auto json = load_fixture("candlesticks.json");
    auto bars = gate::parse_candlesticks_response(json);
    auto frames =
        gate::GateBackfill::to_prepend_frames(bars, "BTC_USDT", "1m");
    ASSERT_EQ(frames.size(), bars.size());

    GateCombinedParser parser;
    for (const auto& f : frames)
    {
        auto evs = parser.parse_records(f);
        ASSERT_EQ(evs.size(), 1u);
        EXPECT_TRUE(std::holds_alternative<provider::bar>(evs[0]));
    }
}

#endif // HAS_GATE
