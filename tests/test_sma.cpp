#include <gtest/gtest.h>
#include "indicator/sma.h"

TEST(SMA, WarmupPeriod)
{
    simple_moving_average sma(3);
    EXPECT_EQ(sma.update(10.0), std::nullopt);
    EXPECT_EQ(sma.update(20.0), std::nullopt);
}

TEST(SMA, FirstValue)
{
    simple_moving_average sma(3);
    sma.update(10.0);
    sma.update(20.0);
    auto val = sma.update(30.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(*val, 20.0);
}

TEST(SMA, SlidingWindow)
{
    simple_moving_average sma(3);
    sma.update(10.0);
    sma.update(20.0);
    sma.update(30.0);
    auto val = sma.update(40.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(*val, 30.0); // (20+30+40)/3
}

TEST(SMA, Period1)
{
    simple_moving_average sma(1);
    auto val = sma.update(42.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(*val, 42.0);
}

TEST(SMA, LargePeriod)
{
    simple_moving_average sma(100);
    for (int i = 0; i < 99; ++i)
        EXPECT_EQ(sma.update(1.0), std::nullopt);
    auto val = sma.update(1.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(*val, 1.0);
}

TEST(SMA, ConstantInput)
{
    simple_moving_average sma(5);
    for (int i = 0; i < 4; ++i)
        sma.update(7.0);
    auto val = sma.update(7.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(*val, 7.0);
}
