#include <gtest/gtest.h>
#include "execution/latency_model.h"

TEST(ZeroLatencyModel, ReturnsZero)
{
    ZeroLatencyModel m;
    EXPECT_EQ(m.get_order_latency().count(), 0);
    EXPECT_EQ(m.get_market_data_latency().count(), 0);
}

TEST(FixedLatencyModel, ReturnsConstant)
{
    FixedLatencyModel m(latency_duration(500), latency_duration(100));
    EXPECT_EQ(m.get_order_latency().count(), 500);
    EXPECT_EQ(m.get_market_data_latency().count(), 100);
    // Call again - should be identical
    EXPECT_EQ(m.get_order_latency().count(), 500);
    EXPECT_EQ(m.get_market_data_latency().count(), 100);
}

TEST(StochasticLatencyModel, NonNegative)
{
    StochasticLatencyModel m(100.0, 50.0, 42);
    for (int i = 0; i < 1000; ++i)
    {
        EXPECT_GE(m.get_order_latency().count(), 0);
        EXPECT_GE(m.get_market_data_latency().count(), 0);
    }
}

TEST(StochasticLatencyModel, Seeded)
{
    StochasticLatencyModel m1(100.0, 50.0, 42);
    StochasticLatencyModel m2(100.0, 50.0, 42);
    for (int i = 0; i < 100; ++i)
        EXPECT_EQ(m1.get_order_latency(), m2.get_order_latency());
}

TEST(StochasticLatencyModel, MeanApproximate)
{
    double target_mean = 500.0;
    StochasticLatencyModel m(target_mean, 100.0, 123);
    double total = 0.0;
    int n = 10000;
    for (int i = 0; i < n; ++i)
        total += static_cast<double>(m.get_order_latency().count());
    double actual_mean = total / n;
    EXPECT_NEAR(actual_mean, target_mean, target_mean * 0.1);
}
