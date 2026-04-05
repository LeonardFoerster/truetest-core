#include <gtest/gtest.h>
#include "indicator/rsi.h"
#include <cmath>

TEST(RSI, WarmupPeriod)
{
    relative_strength_index rsi(3);
    rsi.update(100.0);  // first price, no change yet
    EXPECT_EQ(rsi.update(101.0), std::nullopt);
    EXPECT_EQ(rsi.update(102.0), std::nullopt);
    EXPECT_FALSE(rsi.ready());
}

TEST(RSI, FirstValue)
{
    relative_strength_index rsi(3);
    rsi.update(100.0);
    rsi.update(101.0); // +1
    rsi.update(102.0); // +1
    auto val = rsi.update(103.0); // +1, all gains, no losses
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(*val, 100.0); // all gains → RSI = 100
}

TEST(RSI, AllLosses)
{
    relative_strength_index rsi(3);
    rsi.update(100.0);
    rsi.update(99.0);
    rsi.update(98.0);
    auto val = rsi.update(97.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(*val, 0.0); // all losses → RSI = 0
}

TEST(RSI, MixedMovement)
{
    relative_strength_index rsi(3);
    rsi.update(100.0);
    rsi.update(102.0); // +2
    rsi.update(101.0); // -1
    auto val = rsi.update(103.0); // +2
    ASSERT_TRUE(val.has_value());
    // avg_gain = (2+0+2)/3 = 4/3, avg_loss = (0+1+0)/3 = 1/3
    // RS = (4/3)/(1/3) = 4, RSI = 100 - 100/5 = 80
    EXPECT_NEAR(*val, 80.0, 0.01);
}

TEST(RSI, ConstantPrice)
{
    relative_strength_index rsi(3);
    for (int i = 0; i < 5; ++i)
        rsi.update(50.0);
    // All changes are 0 → avg_gain=0, avg_loss=0 → RSI=100 (no loss)
    ASSERT_TRUE(rsi.ready());
    EXPECT_DOUBLE_EQ(rsi.value(), 100.0);
}

TEST(RSI, DefaultPeriod14)
{
    relative_strength_index rsi; // default period = 14
    // Need 15 prices (1 seed + 14 changes)
    for (int i = 0; i < 14; ++i)
        rsi.update(100.0 + i);
    EXPECT_FALSE(rsi.ready());
    auto val = rsi.update(114.0);
    ASSERT_TRUE(val.has_value());
}
