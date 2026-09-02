// footprint.md §2.1 venue wiring, proven end-to-end: DataBridge tap ->
// SPSC ring -> real §2.2 aggregator -> §2.3 presentation bridge, driven by
// provider::tick/provider::event exactly as a live venue stream would.

#include <gtest/gtest.h>

#include "ui/desk/footprint_live_source.h"

#include <chrono>

#ifdef HAS_BINANCE
#include "providers/binance/binance_combined_parser.h"
#endif

using namespace truetest::ui::desk;
using namespace truetest::footprint;

namespace {

provider::tick make_tick(double price, int64_t qty, uint8_t side)
{
    static std::uint64_t native_id = 0;
    provider::tick t;
    t.timestamp = std::chrono::system_clock::now();
    t.symbol = "BTCUSDT";
    t.price = price;
    t.quantity = qty;
    t.side = side;
    t.native_trade_id = ++native_id;
    return t;
}

FootprintLiveSourceConfig make_config()
{
    FootprintLiveSourceConfig cfg;
    cfg.venue = venue_id::binance_usdm;
    cfg.symbol_id = 1;
    cfg.symbol_label = "BTCUSDT";
    cfg.tick_size = 0.5;
    cfg.qty_atom_scale = 100.0;
    return cfg;
}

} // namespace

TEST(FootprintLiveSource, PollWithNoTradesYetIsUnavailable)
{
    FootprintLiveSource src(make_config());
    auto view = src.poll();
    ASSERT_TRUE(view);
    EXPECT_TRUE(view->footprint.empty());
    EXPECT_EQ(view->state, DeskDataState::unavailable);
}

TEST(FootprintLiveSource, TapThenPollProducesLiveStateAndBackfillingStatus)
{
    FootprintLiveSource src(make_config());
    src.tap_tick(make_tick(100.0, 5, 0));
    src.tap_tick(make_tick(100.5, 3, 1));

    auto view = src.poll();
    ASSERT_TRUE(view);
    ASSERT_FALSE(view->footprint.empty());
    EXPECT_EQ(view->state, DeskDataState::stale);
    // No cache/reconciliation layer exists yet (Phase 2b) - honestly never
    // claims LIVE, always BACKFILLING once trades are flowing.
    EXPECT_EQ(view->footprint_status, data_status::backfilling);
    EXPECT_EQ(src.received_count(), 2u);
}

TEST(FootprintLiveSource, RejectedLateTradeIsVisibleAndNotCounted)
{
    FootprintLiveSource src(make_config());
    auto later = make_tick(100.0, 1, 0);
    later.timestamp = std::chrono::system_clock::time_point(
        std::chrono::seconds(2));
    auto earlier = make_tick(99.0, 1, 1);
    earlier.timestamp = std::chrono::system_clock::time_point(
        std::chrono::seconds(1));
    src.tap_tick(later);
    src.tap_tick(earlier);

    const auto view = src.poll();
    EXPECT_EQ(src.received_count(), 1u);
    EXPECT_EQ(src.rejected_count(), 1u);
    EXPECT_EQ(view->footprint_status, data_status::recovering);
    EXPECT_EQ(view->state, DeskDataState::error);
}

TEST(FootprintLiveSource, TapViaProviderEventVariantRoutesTicksOnly)
{
    FootprintLiveSource src(make_config());
    provider::event tick_ev = make_tick(100.0, 5, 0);
    provider::status status_ev{};
    status_ev.provider = "test";
    provider::event non_tick_ev = status_ev;

    src.tap(tick_ev);
    src.tap(non_tick_ev); // must be silently ignored, not crash

    auto view = src.poll();
    EXPECT_EQ(src.received_count(), 1u);
    EXPECT_FALSE(view->footprint.empty());
}

TEST(FootprintLiveSource, DiscontinuityMarksRecoveringThenBackfillingOnCatchUp)
{
    FootprintLiveSource src(make_config());
    // Overflow the ring (capacity 8192) to force a discontinuity.
    for (int i = 0; i < 8200; ++i)
        src.tap_tick(make_tick(100.0, 1, 0));

    auto view1 = src.poll();
    EXPECT_EQ(view1->footprint_status, data_status::recovering);

    src.tap_tick(make_tick(100.0, 1, 0));
    auto view2 = src.poll();
    EXPECT_EQ(view2->footprint_status, data_status::backfilling);
}

TEST(FootprintLiveSource, VersionMonotonicallyIncreasesAcrossPolls)
{
    FootprintLiveSource src(make_config());
    src.tap_tick(make_tick(100.0, 1, 0));
    const auto v1 = src.poll()->version;
    src.tap_tick(make_tick(100.0, 1, 0));
    const auto v2 = src.poll()->version;
    EXPECT_GT(v2, v1);
}

#ifdef HAS_BINANCE

// The realistic path: raw Binance trade JSON -> BinanceCombinedParser ->
// provider::event -> FootprintLiveSource::tap() -> poll() -> exact ticks
// and a native trade id, with no float ever touching the bucketing.
TEST(FootprintLiveSource, EndToEndFromRawBinanceTradeFrame)
{
    BinanceCombinedParser parser;
    parser.configure_exact_decimal("0.01", 8);

    FootprintLiveSourceConfig cfg;
    cfg.venue = venue_id::binance_usdm;
    cfg.symbol_id = 1;
    cfg.symbol_label = "BTCUSDT";
    cfg.tick_size = 0.01; // must match configure_exact_decimal above
    cfg.qty_atom_scale = 1e8;
    FootprintLiveSource src(std::move(cfg));

    const std::string frame =
        R"({"e":"trade","E":1704067200002,"s":"BTCUSDT","t":555,)"
        R"("p":"68120.50","q":"0.01230000","T":1704067200001,"m":false})";
    auto ev = parser.parse_record(frame);
    ASSERT_TRUE(ev.has_value());

    src.tap(*ev);
    auto view = src.poll();

    ASSERT_FALSE(view->footprint.empty());
    ASSERT_FALSE(view->footprint.front().levels.empty());
    const auto& level = view->footprint.front().levels.front();
    // price_ticks=6812050 * tick_size(0.01) = 68120.50 exactly.
    EXPECT_DOUBLE_EQ(level.price, 68120.50);
    // side "m":false -> aggressor buy (footprint.md §2.1 / binance convention).
    EXPECT_DOUBLE_EQ(level.buy_qty, 0.0123);
    EXPECT_DOUBLE_EQ(level.sell_qty, 0.0);
}

#endif
