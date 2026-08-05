#include <gtest/gtest.h>

#include "analytics/footprint/footprint_suggestions.h"

using namespace truetest::footprint;

TEST(FootprintSuggestions, QuoteVolumeThresholdEmptyIsZero)
{
    EXPECT_DOUBLE_EQ(suggest_quote_volume_threshold({}), 0.0);
}

TEST(FootprintSuggestions, QuoteVolumeThresholdOddCountUsesMedian)
{
    // median of {10,20,30} = 20 -> 20/6
    EXPECT_DOUBLE_EQ(suggest_quote_volume_threshold({30.0, 10.0, 20.0}), 20.0 / 6.0);
}

TEST(FootprintSuggestions, QuoteVolumeThresholdEvenCountAveragesMiddleTwo)
{
    // sorted {10,20,30,40} -> median (20+30)/2=25 -> 25/6
    EXPECT_DOUBLE_EQ(suggest_quote_volume_threshold({40.0, 10.0, 30.0, 20.0}), 25.0 / 6.0);
}

TEST(FootprintSuggestions, ImbalanceMinVolumeEmptyIsZero)
{
    EXPECT_EQ(suggest_imbalance_min_volume({}), 0);
}

TEST(FootprintSuggestions, ImbalanceMinVolumeMedianNoRounding)
{
    EXPECT_EQ(suggest_imbalance_min_volume({30, 10, 20}), 20);
}

TEST(FootprintSuggestions, ImbalanceMinVolumeRoundsDownToQtyStep)
{
    // median 23, step 5 -> round down to 20
    EXPECT_EQ(suggest_imbalance_min_volume({10, 23, 40}, /*qty_step_atoms=*/5), 20);
}

TEST(FootprintSuggestions, ImbalanceMinVolumeStepZeroDisablesRounding)
{
    EXPECT_EQ(suggest_imbalance_min_volume({10, 23, 40}, /*qty_step_atoms=*/0), 23);
}
