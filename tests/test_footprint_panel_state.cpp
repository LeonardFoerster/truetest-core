#include <gtest/gtest.h>

#include "ui/desk/footprint_panel_state.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace truetest::ui::desk;
using BT = FootprintPanelSettings::BarType;

TEST(FootprintPanelSettings, BarTypeIntervalsAreDistinctAndOrdered)
{
    EXPECT_LT(footprint_bar_type_interval_ns(BT::time_1s), footprint_bar_type_interval_ns(BT::time_5s));
    EXPECT_LT(footprint_bar_type_interval_ns(BT::time_5s), footprint_bar_type_interval_ns(BT::time_15s));
    EXPECT_LT(footprint_bar_type_interval_ns(BT::time_15s), footprint_bar_type_interval_ns(BT::time_1m));
    EXPECT_LT(footprint_bar_type_interval_ns(BT::time_1m), footprint_bar_type_interval_ns(BT::time_5m));
}

TEST(FootprintPanelSettings, BarTypeLabelsAreNonEmptyAndDistinct)
{
    const BT types[] = {BT::time_1s, BT::time_5s, BT::time_15s, BT::time_1m, BT::time_5m, BT::volume};
    std::vector<std::string> labels;
    for (auto t : types)
    {
        const char* label = footprint_bar_type_label(t);
        ASSERT_NE(label, nullptr);
        EXPECT_GT(std::string(label).size(), 0u);
        labels.emplace_back(label);
    }
    std::sort(labels.begin(), labels.end());
    EXPECT_EQ(std::adjacent_find(labels.begin(), labels.end()), labels.end()); // all distinct
}

TEST(FootprintPanelSettings, AggregationEqualIgnoresPureDisplayFields)
{
    FootprintPanelSettings a;
    FootprintPanelSettings b = a;
    b.quote_units = !a.quote_units;
    b.cvd_collapsed = !a.cvd_collapsed;
    EXPECT_TRUE(a.aggregation_equal(b));
}

TEST(FootprintPanelSettings, AggregationEqualDetectsBarTypeChange)
{
    FootprintPanelSettings a;
    FootprintPanelSettings b = a;
    b.bar_type = BT::time_5m;
    EXPECT_FALSE(a.aggregation_equal(b));
}

TEST(FootprintPanelSettings, AggregationEqualDetectsTickGroupChange)
{
    FootprintPanelSettings a;
    FootprintPanelSettings b = a;
    b.tick_group = a.tick_group + 1;
    EXPECT_FALSE(a.aggregation_equal(b));
}

TEST(FootprintDemoStateTest, ConstructsWithNonEmptyDeterministicBars)
{
    FootprintDemoState demo;
    ASSERT_FALSE(demo.aggregator.bars().empty());
    ASSERT_FALSE(demo.trades.empty());

    // Same settings -> reaggregating must reproduce identical bar count and
    // CVD (determinism of the demo trade generator + aggregator).
    const auto bars_before = demo.aggregator.bars().size();
    const auto cvd_before = demo.aggregator.cvd();
    demo.reaggregate();
    EXPECT_EQ(demo.aggregator.bars().size(), bars_before);
    EXPECT_EQ(demo.aggregator.cvd(), cvd_before);
}

TEST(FootprintDemoStateTest, ChangingBarTypeAndReaggregatingChangesBarCount)
{
    FootprintDemoState demo;
    const auto bars_1m = demo.aggregator.bars().size();

    demo.settings.bar_type = BT::time_5m; // fewer, wider bars over the same trades
    demo.reaggregate();
    const auto bars_5m = demo.aggregator.bars().size();

    EXPECT_LT(bars_5m, bars_1m);
}
