#include <gtest/gtest.h>
#include "execution/fee_model.h"

TEST(ZeroFeeModel, ReturnsZero)
{
    ZeroFeeModel m;
    EXPECT_DOUBLE_EQ(m.compute_commission(order_side::buy, 100, 50.0), 0.0);
    EXPECT_DOUBLE_EQ(m.compute_commission(order_side::sell, 1, 10000.0), 0.0);
}

TEST(FixedFeeModel, IgnoresQuantityAndPrice)
{
    FixedFeeModel m(5.0);
    EXPECT_DOUBLE_EQ(m.compute_commission(order_side::buy, 1, 1.0), 5.0);
    EXPECT_DOUBLE_EQ(m.compute_commission(order_side::sell, 10000, 999.0), 5.0);
}

TEST(TieredFeeModel, BuyAppliesTaker)
{
    TieredFeeModel m(0.001, 0.002);
    // Buy 100@50 = notional 5000, taker rate 0.002 → 10.0
    EXPECT_DOUBLE_EQ(m.compute_commission(order_side::buy, 100, 50.0), 10.0);
}

TEST(TieredFeeModel, SellAppliesMaker)
{
    TieredFeeModel m(0.001, 0.002);
    // Sell 100@50 = notional 5000, maker rate 0.001 → 5.0
    EXPECT_DOUBLE_EQ(m.compute_commission(order_side::sell, 100, 50.0), 5.0);
}

TEST(TieredFeeModel, ZeroQuantity)
{
    TieredFeeModel m(0.001, 0.002);
    EXPECT_DOUBLE_EQ(m.compute_commission(order_side::buy, 0, 50.0), 0.0);
}

TEST(TieredFeeModel, LargeNotional)
{
    TieredFeeModel m(0.001, 0.002);
    double commission = m.compute_commission(order_side::buy, 1000000, 1000.0);
    EXPECT_NEAR(commission, 2000000.0, 0.01);
}
