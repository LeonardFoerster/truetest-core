#include <gtest/gtest.h>
#include "indicator/ema.h"

TEST(EMA, WarmupPeriod)
{
    exponential_moving_average ema(3);
    EXPECT_EQ(ema.update(10.0), std::nullopt);
    EXPECT_EQ(ema.update(20.0), std::nullopt);
    EXPECT_FALSE(ema.ready());
}

TEST(EMA, FirstValue)
{
    exponential_moving_average ema(3);
    ema.update(10.0);
    ema.update(20.0);
    auto val = ema.update(30.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(*val, 20.0); // SMA seed: (10+20+30)/3
}

TEST(EMA, SubsequentValues)
{
    exponential_moving_average ema(3);
    ema.update(10.0);
    ema.update(20.0);
    ema.update(30.0); // EMA = 20.0
    auto val = ema.update(40.0);
    ASSERT_TRUE(val.has_value());
    // k = 2/(3+1) = 0.5, EMA = 40*0.5 + 20*0.5 = 30
    EXPECT_DOUBLE_EQ(*val, 30.0);
}

TEST(EMA, Period1)
{
    exponential_moving_average ema(1);
    auto val = ema.update(42.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(*val, 42.0);
}

TEST(EMA, ConstantInput)
{
    exponential_moving_average ema(5);
    for (int i = 0; i < 4; ++i)
        ema.update(7.0);
    auto val = ema.update(7.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(*val, 7.0);
    val = ema.update(7.0);
    EXPECT_DOUBLE_EQ(*val, 7.0);
}
