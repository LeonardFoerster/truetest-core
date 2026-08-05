// footprint.md §2.3 "immutable FootprintPresentation snapshots" - here
// realized as a bridge from the §2.2 FootprintAggregator into the desk's
// existing FootprintBarView seam (research_views.h).

#include <gtest/gtest.h>

#include "ui/desk/footprint_presentation_bridge.h"

using namespace truetest::footprint;
using namespace truetest::ui::desk;

namespace {

constexpr std::int64_t kSecond = 1'000'000'000LL;
constexpr std::int64_t kMinute = 60 * kSecond;

PublicTrade make_trade(std::int64_t price_ticks, std::int64_t qty_atoms,
                        aggressor_side side, std::int64_t event_ns)
{
    PublicTrade t;
    t.event_ns = event_ns;
    t.recv_ns = event_ns;
    t.price_ticks = price_ticks;
    t.base_qty_atoms = qty_atoms;
    t.side = side;
    return t;
}

} // namespace

TEST(FootprintPresentationBridge, EmptyAggregatorProducesEmptyViews)
{
    FootprintAggregatorConfig cfg;
    cfg.tick_size = 0.5;
    FootprintAggregator agg(cfg);
    EXPECT_TRUE(to_footprint_bar_views(agg).empty());
}

TEST(FootprintPresentationBridge, ConvertsOhlcAndStateAndCvd)
{
    FootprintAggregatorConfig cfg;
    cfg.tick_size = 0.5; // price_ticks 200 -> 100.0
    cfg.qty_atom_scale = 10.0; // 10 atoms per whole unit
    FootprintAggregator agg(cfg);
    agg.on_trade(make_trade(200, 50, aggressor_side::buy, 0));  // price 100.0, qty 5
    agg.on_trade(make_trade(202, 30, aggressor_side::sell, kSecond)); // price 101.0, qty 3
    agg.flush();

    const auto views = to_footprint_bar_views(agg);
    ASSERT_EQ(views.size(), 1u);
    const auto& v = views[0];
    EXPECT_EQ(v.state, FootprintBarState::complete);
    EXPECT_DOUBLE_EQ(v.open, 100.0);
    EXPECT_DOUBLE_EQ(v.high, 101.0);
    EXPECT_DOUBLE_EQ(v.low, 100.0);
    EXPECT_DOUBLE_EQ(v.close, 101.0);
    EXPECT_DOUBLE_EQ(v.cvd, 2.0); // 5 buy - 3 sell
    EXPECT_FALSE(v.gap);
}

TEST(FootprintPresentationBridge, LevelsSortedByPriceAscending)
{
    FootprintAggregatorConfig cfg;
    cfg.tick_size = 1.0;
    FootprintAggregator agg(cfg);
    agg.on_trade(make_trade(105, 1, aggressor_side::buy, 0));
    agg.on_trade(make_trade(100, 1, aggressor_side::buy, kSecond));
    agg.on_trade(make_trade(103, 1, aggressor_side::buy, 2 * kSecond));
    agg.flush();

    const auto views = to_footprint_bar_views(agg);
    ASSERT_EQ(views[0].levels.size(), 3u);
    EXPECT_LT(views[0].levels[0].price, views[0].levels[1].price);
    EXPECT_LT(views[0].levels[1].price, views[0].levels[2].price);
    EXPECT_DOUBLE_EQ(views[0].levels[0].price, 100.0);
}

TEST(FootprintPresentationBridge, MarksPocLevelAndImbalanceAndStacked)
{
    FootprintAggregatorConfig cfg;
    cfg.tick_size = 1.0;
    cfg.imbalance_min_volume = 10;
    FootprintAggregator agg(cfg);
    agg.on_trade(make_trade(99, 10, aggressor_side::sell, 0));
    agg.on_trade(make_trade(100, 30, aggressor_side::buy, kSecond)); // POC + diagonal buy
    agg.flush();

    const auto views = to_footprint_bar_views(agg);
    bool found_poc = false;
    for (const auto& lv : views[0].levels)
    {
        if (lv.price == 100.0)
        {
            EXPECT_TRUE(lv.is_poc);
            EXPECT_EQ(lv.diagonal, FootprintImbalance::buy);
            found_poc = true;
        }
        else
        {
            EXPECT_FALSE(lv.is_poc);
        }
    }
    EXPECT_TRUE(found_poc);
}

TEST(FootprintPresentationBridge, BaseUnitsVsQuoteUnitsToggle)
{
    FootprintAggregatorConfig cfg;
    cfg.tick_size = 2.0; // price_ticks 50 -> 100.0
    cfg.qty_atom_scale = 1.0;
    FootprintAggregator agg(cfg);
    agg.on_trade(make_trade(50, 5, aggressor_side::buy, 0)); // price 100.0, qty 5

    FootprintPresentationOptions base_opts;
    base_opts.quote_units = false;
    const auto base_views = to_footprint_bar_views(agg, base_opts);
    EXPECT_DOUBLE_EQ(base_views[0].levels[0].buy_qty, 5.0);

    FootprintPresentationOptions quote_opts;
    quote_opts.quote_units = true;
    const auto quote_views = to_footprint_bar_views(agg, quote_opts);
    EXPECT_DOUBLE_EQ(quote_views[0].levels[0].buy_qty, 500.0); // 5 * 100.0
}

TEST(FootprintPresentationBridge, EmptyBarStateSurfacedHonestly)
{
    FootprintAggregatorConfig cfg;
    cfg.tick_size = 1.0;
    FootprintAggregator agg(cfg);
    agg.on_trade(make_trade(100, 1, aggressor_side::buy, 0));
    agg.on_trade(make_trade(100, 1, aggressor_side::buy, 2 * kMinute)); // skips one minute

    const auto views = to_footprint_bar_views(agg);
    ASSERT_EQ(views.size(), 3u);
    EXPECT_EQ(views[1].state, FootprintBarState::empty);
    EXPECT_TRUE(views[1].levels.empty());
}

TEST(FootprintPresentationBridge, UnknownAggressionSurfacedSeparately)
{
    FootprintAggregatorConfig cfg;
    cfg.tick_size = 1.0;
    FootprintAggregator agg(cfg);
    agg.on_trade(make_trade(100, 7, aggressor_side::unknown, 0));
    agg.flush();

    const auto views = to_footprint_bar_views(agg);
    ASSERT_EQ(views[0].levels.size(), 1u);
    EXPECT_DOUBLE_EQ(views[0].levels[0].unknown_qty, 7.0);
    EXPECT_DOUBLE_EQ(views[0].levels[0].buy_qty, 0.0);
    EXPECT_DOUBLE_EQ(views[0].levels[0].sell_qty, 0.0);
}
