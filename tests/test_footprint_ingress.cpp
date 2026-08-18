// Phase 1 tests for the footprint public-trade ingress plumbing
// (footprint.md §2.1): PublicTrade enrichment on provider::tick, tick-size
// resolution, the tick -> PublicTrade research tap, the bounded SPSC
// research ring, and the cold-path consumer service.

#include <gtest/gtest.h>

#include "helpers/alloc_counter.h"
#include "helpers/mock_transport.h"

#include "data/data_handler.h"
#include "providers/data_bridge.h"
#include "providers/footprint/footprint_research_service.h"
#include "providers/footprint/footprint_research_tap.h"
#include "providers/footprint/footprint_ring.h"
#include "providers/footprint/footprint_venue_capabilities.h"
#include "providers/provider_convert.h"
#include "providers/provider_event.h"
#include "types/public_trade.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

using namespace truetest::footprint;

namespace {

provider::tick make_tick(double price, int64_t qty, uint8_t side,
                          int64_t epoch_ns = 1'000'000'000)
{
    provider::tick t;
    t.timestamp = std::chrono::system_clock::time_point(
        std::chrono::nanoseconds(epoch_ns));
    t.symbol = "BTCUSDT";
    t.price = price;
    t.quantity = qty;
    t.side = side;
    return t;
}

// Minimal "price,qty,side" line parser so the DataBridge<provider::tick>
// integration test below exercises the real streaming path without pulling
// in a full venue parser. A malformed/header line (no commas) fails to
// split and correctly yields no record, matching how the existing
// DataBridge streaming tests treat their header line.
class MockTickLineParser : public IDataParser<provider::tick>
{
public:
    std::optional<provider::tick> parse_record(const std::string& line) override
    {
        std::istringstream iss(line);
        std::string price_s, qty_s, side_s;
        if (!std::getline(iss, price_s, ',')) return std::nullopt;
        if (!std::getline(iss, qty_s, ',')) return std::nullopt;
        if (!std::getline(iss, side_s, ',')) return std::nullopt;

        provider::tick t;
        t.timestamp = std::chrono::system_clock::now();
        t.symbol = "BTCUSDT";
        t.price = std::stod(price_s);
        t.quantity = std::stoll(qty_s);
        t.side = static_cast<uint8_t>(std::stoi(side_s));
        return t;
    }

    empty_parse_status classify_empty_frame(std::string_view line) const override
    {
        return line == "header" ? empty_parse_status::ignored
                                : empty_parse_status::malformed;
    }
};

} // namespace

// --- provider::tick enrichment stays additive / backward compatible ---

TEST(FootprintIngress, TickEnrichmentDefaultsToAbsent)
{
    provider::tick t = make_tick(100.0, 5, 0);
    EXPECT_EQ(t.native_trade_id, 0u);
    EXPECT_EQ(t.price_ticks, 0);
    EXPECT_EQ(t.base_qty_atoms, 0);
    EXPECT_FALSE(t.has_exact_decimal);
}

TEST(FootprintIngress, TickEnrichmentIgnoredAndQuantityScalePreservedByConversion)
{
    provider::tick t = make_tick(100.25, 7, 1);
    t.native_trade_id = 123456;
    t.price_ticks = 1002500;
    t.base_qty_atoms = 700000000;
    t.has_exact_decimal = true;
    t.quantity_scale = 100'000'000ULL;

    // Existing engine conversion path must be untouched by the enrichment.
    auto rec = provider::to_tick_record(t);
    EXPECT_DOUBLE_EQ(rec.price, 100.25);
    EXPECT_EQ(rec.quantity, 7);
    EXPECT_EQ(rec.side, data_tick_side::ask);
    EXPECT_EQ(rec.quantity_scale, 100'000'000ULL);

    auto round_trip = provider::from_tick_record(rec);
    EXPECT_EQ(round_trip.quantity_scale, 100'000'000ULL);
}

// --- Tick size resolution: metadata wins, override only when metadata
// absent, conflicts refuse rather than guess (§2.1) ---

TEST(FootprintTickSize, MetadataOnlyResolves)
{
    auto r = resolve_footprint_tick_size(0.1, std::nullopt);
    EXPECT_EQ(r.status, tick_size_status::resolved);
    EXPECT_DOUBLE_EQ(r.tick_size, 0.1);
}

TEST(FootprintTickSize, OverrideOnlyResolvesWhenMetadataAbsent)
{
    auto r = resolve_footprint_tick_size(std::nullopt, 0.5);
    EXPECT_EQ(r.status, tick_size_status::resolved);
    EXPECT_DOUBLE_EQ(r.tick_size, 0.5);
}

TEST(FootprintTickSize, AgreeingOverrideResolves)
{
    auto r = resolve_footprint_tick_size(0.1, 0.1);
    EXPECT_EQ(r.status, tick_size_status::resolved);
    EXPECT_DOUBLE_EQ(r.tick_size, 0.1);
}

TEST(FootprintTickSize, ConflictingOverrideRefuses)
{
    auto r = resolve_footprint_tick_size(0.1, 0.5);
    EXPECT_EQ(r.status, tick_size_status::conflicting_override);
}

TEST(FootprintTickSize, InvalidOverrideRefuses)
{
    auto r = resolve_footprint_tick_size(std::nullopt, -1.0);
    EXPECT_EQ(r.status, tick_size_status::invalid_override);

    auto r2 = resolve_footprint_tick_size(std::nullopt, 0.0);
    EXPECT_EQ(r2.status, tick_size_status::invalid_override);
}

TEST(FootprintTickSize, NeitherSourceIsUnavailable)
{
    auto r = resolve_footprint_tick_size(std::nullopt, std::nullopt);
    EXPECT_EQ(r.status, tick_size_status::unavailable);
}

TEST(FootprintTickSize, NonPositiveMetadataTreatedAsAbsent)
{
    // Metadata of 0 or negative is not a usable tick size - falls through
    // to the override rather than "resolving" to a nonsense tick.
    auto r = resolve_footprint_tick_size(0.0, 0.25);
    EXPECT_EQ(r.status, tick_size_status::resolved);
    EXPECT_DOUBLE_EQ(r.tick_size, 0.25);
}

// --- tick_to_public_trade conversion ---

TEST(FootprintTap, ExactDecimalTickUsedVerbatimNoFloatingPoint)
{
    FootprintTapContext ctx;
    ctx.venue = venue_id::binance_usdm;
    ctx.symbol_id = 7;
    ctx.session_id = 42;
    ctx.tick_size = 0.0; // deliberately unresolved - must not matter here

    provider::tick t = make_tick(123.45, 9, 0);
    t.native_trade_id = 555;
    t.price_ticks = 1234500;
    t.base_qty_atoms = 900000000;
    t.has_exact_decimal = true;

    PublicTrade pt = tick_to_public_trade(ctx, t);
    EXPECT_EQ(pt.price_ticks, 1234500);
    EXPECT_EQ(pt.base_qty_atoms, 900000000);
    EXPECT_EQ(pt.native_trade_id, 555u);
    EXPECT_EQ(pt.venue_id, static_cast<std::uint16_t>(venue_id::binance_usdm));
    EXPECT_EQ(pt.symbol_id, 7u);
    EXPECT_EQ(pt.session_id, 42u);
    EXPECT_EQ(pt.side, aggressor_side::buy);
    EXPECT_TRUE(pt.flags & provenance_native_id);
    EXPECT_FALSE(pt.flags & provenance_session_only);
}

TEST(FootprintTap, FallbackDerivesFromTickSizeWhenNoExactDecimal)
{
    FootprintTapContext ctx;
    ctx.tick_size = 0.5;
    ctx.qty_atom_scale = 100.0;

    provider::tick t = make_tick(100.0, 3, 1); // sell aggressor
    PublicTrade pt = tick_to_public_trade(ctx, t);

    EXPECT_EQ(pt.price_ticks, 200); // 100.0 / 0.5
    EXPECT_EQ(pt.base_qty_atoms, 300); // 3 * 100
    EXPECT_EQ(pt.side, aggressor_side::sell);
    EXPECT_EQ(pt.native_trade_id, 0u);
    EXPECT_TRUE(pt.flags & provenance_session_only);
}

TEST(FootprintTap, FallbackNormalizesFixedPointProviderQuantity)
{
    FootprintTapContext ctx;
    ctx.tick_size = 0.5;
    ctx.qty_atom_scale = 100'000'000.0;

    provider::tick t = make_tick(100.0, 25'000'000, 0);
    t.quantity_scale = 100'000'000ULL; // 0.25 base units
    PublicTrade pt = tick_to_public_trade(ctx, t);

    EXPECT_EQ(pt.base_qty_atoms, 25'000'000);
}

TEST(FootprintTap, UnknownSideMapsToUnknown)
{
    FootprintTapContext ctx;
    ctx.tick_size = 1.0;
    provider::tick t = make_tick(10.0, 1, 2);
    PublicTrade pt = tick_to_public_trade(ctx, t);
    EXPECT_EQ(pt.side, aggressor_side::unknown);
}

// --- FootprintResearchRing: SPSC semantics + discontinuity marking ---

TEST(FootprintRing, PushPopFifoOrder)
{
    FootprintResearchRing<8> ring;
    PublicTrade a; a.obs_seq = 1;
    PublicTrade b; b.obs_seq = 2;
    EXPECT_TRUE(ring.try_push(a));
    EXPECT_TRUE(ring.try_push(b));

    PublicTrade out;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out.obs_seq, 1u);
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out.obs_seq, 2u);
    EXPECT_FALSE(ring.try_pop(out));
}

TEST(FootprintRing, OverflowDropsAndMarksDiscontinuousWithoutBlocking)
{
    FootprintResearchRing<4> ring;
    for (int i = 0; i < 4; ++i)
    {
        PublicTrade t; t.obs_seq = static_cast<uint64_t>(i);
        EXPECT_TRUE(ring.try_push(t));
    }
    EXPECT_FALSE(ring.discontinuous());

    PublicTrade overflow; overflow.obs_seq = 999;
    EXPECT_FALSE(ring.try_push(overflow)); // dropped, not blocked
    EXPECT_TRUE(ring.discontinuous());
    EXPECT_EQ(ring.discontinuity_count(), 1u);

    // The four already-queued trades are still intact and in order - the
    // drop must not corrupt the existing queue (§2.1: "never halt or slow
    // the engine", and reconciliation relies on a clean prefix).
    PublicTrade out;
    for (uint64_t i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out.obs_seq, i);
    }
}

TEST(FootprintRing, AcknowledgeClearsDiscontinuousFlag)
{
    FootprintResearchRing<2> ring;
    ring.try_push(PublicTrade{});
    ring.try_push(PublicTrade{});
    EXPECT_FALSE(ring.try_push(PublicTrade{}));
    ASSERT_TRUE(ring.discontinuous());

    ring.acknowledge_discontinuity();
    EXPECT_FALSE(ring.discontinuous());
    // Count is a running total - acknowledging clears the sticky flag, not
    // the historical counter.
    EXPECT_EQ(ring.discontinuity_count(), 1u);
}

// --- try_tap_push: end-to-end tap -> ring ---

TEST(FootprintTap, PushesConvertedTradeIntoRing)
{
    FootprintResearchRing<8> ring;
    FootprintTapContext ctx;
    ctx.venue = venue_id::bitunix_futures;
    ctx.symbol_id = 3;
    ctx.session_id = 11;
    ctx.tick_size = 0.1;

    provider::tick t = make_tick(200.3, 2, 0);
    EXPECT_TRUE(try_tap_push(ctx, t, ring));

    PublicTrade out;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out.price_ticks, 2003);
    EXPECT_EQ(out.symbol_id, 3u);
    EXPECT_EQ(out.session_id, 11u);
    EXPECT_EQ(out.obs_seq, 0u); // first call, ctx.next_obs_seq started at 0
}

TEST(FootprintTap, ObsSeqMonotonicPerContext)
{
    FootprintResearchRing<8> ring;
    FootprintTapContext ctx;
    ctx.tick_size = 1.0;

    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(try_tap_push(ctx, make_tick(10.0 + i, 1, 0), ring));

    PublicTrade out;
    for (uint64_t i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out.obs_seq, i);
    }
}

TEST(FootprintTap, RefusesWhenTickSizeUnresolvedAndNoExactDecimal)
{
    FootprintResearchRing<8> ring;
    FootprintTapContext ctx; // tick_size stays 0.0 - "unavailable"

    provider::tick t = make_tick(50.0, 1, 0);
    EXPECT_FALSE(try_tap_push(ctx, t, ring));
    EXPECT_EQ(ring.size(), 0u);
    EXPECT_FALSE(ring.discontinuous()); // refusal is not the same as a drop
}

TEST(FootprintTap, ExactDecimalTickDoesNotNeedResolvedTickSize)
{
    FootprintResearchRing<8> ring;
    FootprintTapContext ctx; // tick_size unresolved

    provider::tick t = make_tick(50.0, 1, 0);
    t.has_exact_decimal = true;
    t.price_ticks = 500;
    t.base_qty_atoms = 100000000;

    EXPECT_TRUE(try_tap_push(ctx, t, ring));
    EXPECT_EQ(ring.size(), 1u);
}

// --- FootprintResearchService: bounded working-set consumer ---

TEST(FootprintService, DrainOnceMovesTradesIntoWorkingSet)
{
    FootprintResearchRing<16> ring;
    FootprintResearchService<16, 8> svc(ring);

    for (int i = 0; i < 5; ++i)
    {
        PublicTrade t; t.obs_seq = static_cast<uint64_t>(i);
        ASSERT_TRUE(ring.try_push(t));
    }

    EXPECT_EQ(svc.drain_once(), 5u);
    EXPECT_EQ(svc.received_count(), 5u);
    EXPECT_EQ(svc.working_set_size(), 5u);
    for (std::size_t i = 0; i < 5; ++i)
        EXPECT_EQ(svc.working_set_at(i).obs_seq, i);

    // Ring is now empty - a second drain is a no-op, not an error.
    EXPECT_EQ(svc.drain_once(), 0u);
}

TEST(FootprintService, WorkingSetIsBoundedAndKeepsMostRecent)
{
    FootprintResearchRing<32> ring;
    FootprintResearchService<32, 4> svc(ring); // working set smaller than input

    for (int i = 0; i < 10; ++i)
    {
        PublicTrade t; t.obs_seq = static_cast<uint64_t>(i);
        ASSERT_TRUE(ring.try_push(t));
    }
    svc.drain_once();

    ASSERT_EQ(svc.working_set_size(), 4u);
    // Oldest retained is obs_seq 6 (10 pushed, last 4 kept: 6,7,8,9).
    EXPECT_EQ(svc.working_set_at(0).obs_seq, 6u);
    EXPECT_EQ(svc.working_set_at(3).obs_seq, 9u);
}

TEST(FootprintService, RingDiscontinuityMovesStatusToRecoveringAndAcks)
{
    FootprintResearchRing<2> ring;
    FootprintResearchService<2, 8> svc(ring);

    ring.try_push(PublicTrade{});
    ring.try_push(PublicTrade{});
    EXPECT_FALSE(ring.try_push(PublicTrade{})); // drop -> discontinuous
    ASSERT_TRUE(ring.discontinuous());

    svc.drain_once();
    EXPECT_EQ(svc.status(), data_status::recovering);
    EXPECT_EQ(svc.discontinuity_events(), 1u);
    // Service acknowledges on the producer's behalf during drain.
    EXPECT_FALSE(ring.discontinuous());
}

TEST(FootprintService, DrainRespectsMaxItemsCap)
{
    FootprintResearchRing<16> ring;
    FootprintResearchService<16, 16> svc(ring);
    for (int i = 0; i < 6; ++i)
        ring.try_push(PublicTrade{});

    EXPECT_EQ(svc.drain_once(4), 4u);
    EXPECT_EQ(ring.size(), 2u);
}

// --- Hot-path adjacency: the tap + ring push path must not allocate ---
// (footprint.md §2.1 "must not allocate ... on the DataBridge research
// tap"; this is off the true engine hot path but shares the same
// zero-alloc discipline, so it is measured the same way as
// tests/test_hotpath_allocs.cpp.)

TEST(FootprintHotpath, TapPushSteadyStateIsAllocationFree)
{
    FootprintResearchRing<1024> ring;
    FootprintTapContext ctx;
    ctx.venue = venue_id::binance_usdm;
    ctx.symbol_id = 1;
    ctx.tick_size = 0.01;

    provider::tick t = make_tick(100.0, 1, 0);

    // Warm up (first-touch paging, etc.) before measuring.
    for (int i = 0; i < 16; ++i)
        try_tap_push(ctx, t, ring);
    PublicTrade discard;
    while (ring.try_pop(discard)) {}

    truetest::test::alloc::measure_window window;
    for (int i = 0; i < 512; ++i)
    {
        ASSERT_TRUE(try_tap_push(ctx, t, ring));
        PublicTrade out;
        ASSERT_TRUE(ring.try_pop(out));
    }
    const auto delta = window.total();
    EXPECT_EQ(delta.count, 0u);
    EXPECT_EQ(delta.bytes, 0u);
}

TEST(FootprintHotpath, DataBridgeResearchTapHookDefaultsToNullopNoOverhead)
{
    // Constructing/using a DataBridge without installing a research tap must
    // compile and behave exactly as before (opt-in only, §2.1). This is a
    // compile+link smoke test - full streaming behavior is covered by
    // existing DataBridge tests.
    using Bridge = DataBridge<provider::tick>;
    static_assert(std::is_same_v<
        decltype(std::declval<Bridge>().set_research_tap(nullptr)), void>);
    SUCCEED();
}

// End-to-end: a real DataBridge<provider::tick> streaming session, with a
// research tap installed that runs the actual try_tap_push() production
// path, must deliver every streamed tick into the ring - exercising the
// `if (research_tap_) research_tap_(record);` call site in data_bridge.h
// itself, not just the tap function in isolation.
TEST(FootprintDataBridgeIntegration, ResearchTapDeliversEveryStreamedTickIntoRing)
{
    auto transport = std::make_shared<MockStreamingTransport>();
    auto parser = std::make_shared<MockTickLineParser>();
    auto bridge = std::make_shared<DataBridge<provider::tick>>(transport, parser);

    FootprintResearchRing<64> ring;
    FootprintTapContext ctx;
    ctx.venue = venue_id::binance_usdm;
    ctx.symbol_id = 5;
    ctx.session_id = 77;
    ctx.tick_size = 0.5;

    bridge->set_research_tap([&](const provider::tick& t) {
        try_tap_push(ctx, t, ring);
    });

    const auto before_call_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::thread feeder([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        transport->enqueue("header"); // no commas -> parses to no record
        for (int i = 0; i < 4; ++i)
            transport->enqueue(std::to_string(100.0 + i) + ",2,0");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        transport->request_stop();
    });

    auto dh = std::make_shared<data_handler>();
    bridge->run_streaming(dh);
    feeder.join();

    FootprintResearchService<64> svc(ring);
    ASSERT_EQ(svc.drain_once(), 4u);
    for (std::size_t i = 0; i < 4; ++i)
    {
        const auto& pt = svc.working_set_at(i);
        EXPECT_EQ(pt.symbol_id, 5u);
        EXPECT_EQ(pt.venue_id, static_cast<std::uint16_t>(venue_id::binance_usdm));
        EXPECT_EQ(pt.session_id, 77u);
        EXPECT_EQ(pt.side, aggressor_side::buy);
        EXPECT_EQ(pt.obs_seq, i);
        // recv_ns is the local clock sampled inside try_tap_push - it must
        // be a real "now" reading, not aliased to event_ns/zero.
        EXPECT_GE(pt.recv_ns, before_call_ns);
    }
    EXPECT_EQ(svc.working_set_at(0).price_ticks, 200); // 100.0 / 0.5
}
