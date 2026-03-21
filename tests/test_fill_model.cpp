#include <gtest/gtest.h>
#include "orderbook/fill_model.h"

TEST(PerfectFillModel, ZeroFade)
{
    PerfectFillModel m;
    EXPECT_DOUBLE_EQ(m.get_fade_rate(), 0.0);
}

TEST(PerfectFillModel, AlwaysFills)
{
    PerfectFillModel m;
    EXPECT_DOUBLE_EQ(m.get_fill_probability(order_side::buy, 0.0), 1.0);
    EXPECT_DOUBLE_EQ(m.get_fill_probability(order_side::sell, 0.05), 1.0);
    EXPECT_DOUBLE_EQ(m.get_fill_probability(order_side::buy, 1.0), 1.0);
}

TEST(RealisticFillModel, FadeRate)
{
    RealisticFillModel m(0.1);
    EXPECT_DOUBLE_EQ(m.get_fade_rate(), 0.1);
}

TEST(RealisticFillModel, AtMoney)
{
    RealisticFillModel m(0.1, 0.95, 10.0);
    EXPECT_DOUBLE_EQ(m.get_fill_probability(order_side::buy, 0.0), 0.95);
}

TEST(RealisticFillModel, FarFromMid)
{
    RealisticFillModel m(0.1, 0.95, 10.0);
    double prob = m.get_fill_probability(order_side::buy, 0.1);
    EXPECT_LT(prob, 0.95);
    EXPECT_GT(prob, 0.0);
}

TEST(RealisticFillModel, ProbDecreases)
{
    RealisticFillModel m(0.1, 0.95, 10.0);
    double p1 = m.get_fill_probability(order_side::buy, 0.01);
    double p2 = m.get_fill_probability(order_side::buy, 0.05);
    double p3 = m.get_fill_probability(order_side::buy, 0.10);
    EXPECT_GT(p1, p2);
    EXPECT_GT(p2, p3);
}

TEST(RealisticFillModel, ClampFadeRate)
{
    RealisticFillModel m(1.5);
    EXPECT_DOUBLE_EQ(m.get_fade_rate(), 1.0);
}

TEST(RealisticFillModel, ClampBaseProbability)
{
    RealisticFillModel m(0.1, 2.0, 10.0);
    EXPECT_DOUBLE_EQ(m.get_fill_probability(order_side::buy, 0.0), 1.0);
}
