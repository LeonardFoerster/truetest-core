#include <gtest/gtest.h>
#include "indicator/bollinger.h"
#include <cmath>

TEST(Bollinger, WarmupPeriod)
{
    bollinger_bands bb(3);
    EXPECT_EQ(bb.update(10.0), std::nullopt);
    EXPECT_EQ(bb.update(20.0), std::nullopt);
    EXPECT_FALSE(bb.ready());
}

TEST(Bollinger, FirstValue)
{
    bollinger_bands bb(3, 2.0);
    bb.update(10.0);
    bb.update(20.0);
    auto val = bb.update(30.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(val->middle, 20.0);
    // stddev = sqrt(((100+400+900)/3) - 400) = sqrt(200/3) ≈ 8.165
    double expected_std = std::sqrt(200.0 / 3.0);
    EXPECT_NEAR(val->upper, 20.0 + 2.0 * expected_std, 0.001);
    EXPECT_NEAR(val->lower, 20.0 - 2.0 * expected_std, 0.001);
}

TEST(Bollinger, ConstantInput)
{
    bollinger_bands bb(3, 2.0);
    bb.update(5.0);
    bb.update(5.0);
    auto val = bb.update(5.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(val->middle, 5.0);
    // stddev = 0 → bands collapse to middle
    EXPECT_DOUBLE_EQ(val->upper, 5.0);
    EXPECT_DOUBLE_EQ(val->lower, 5.0);
}

TEST(Bollinger, SlidingWindow)
{
    bollinger_bands bb(3, 1.0);
    bb.update(10.0);
    bb.update(20.0);
    bb.update(30.0);
    auto val = bb.update(40.0); // window: 20, 30, 40
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(val->middle, 30.0);
}

TEST(Bollinger, DefaultParams)
{
    bollinger_bands bb; // period=20, num_std=2
    for (int i = 0; i < 19; ++i)
        bb.update(100.0);
    EXPECT_FALSE(bb.ready());
    auto val = bb.update(100.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(val->middle, 100.0);
}
