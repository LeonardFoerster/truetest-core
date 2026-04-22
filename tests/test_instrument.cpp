#include <gtest/gtest.h>
#include "execution/instrument.h"

TEST(Instrument, QuantizePriceToTick_RoundsToNearest)
{
    EXPECT_DOUBLE_EQ(quantize_price_to_tick(100.123, 0.01), 100.12);
    EXPECT_DOUBLE_EQ(quantize_price_to_tick(100.126, 0.01), 100.13);
    EXPECT_DOUBLE_EQ(quantize_price_to_tick(100.125, 0.05), 100.15);
    EXPECT_NEAR      (quantize_price_to_tick(49999.9, 10.0), 50000.0, 1e-9);
}

TEST(Instrument, QuantizePriceToTick_ZeroTickPassThrough)
{
    EXPECT_DOUBLE_EQ(quantize_price_to_tick(123.456, 0.0), 123.456);
    EXPECT_DOUBLE_EQ(quantize_price_to_tick(123.456, -1.0), 123.456);
}

TEST(Instrument, FloorQtyToLot_NeverExceedsInput)
{
    EXPECT_DOUBLE_EQ(floor_qty_to_lot(0.00015, 0.0001), 0.0001);
    EXPECT_DOUBLE_EQ(floor_qty_to_lot(1.9, 1.0), 1.0);
    EXPECT_DOUBLE_EQ(floor_qty_to_lot(5.0, 1.0), 5.0);
}

TEST(Instrument, MeetsMinQty_UnsetAllowsAll)
{
    EXPECT_TRUE(meets_min_qty(0.0, 0.0));
    EXPECT_TRUE(meets_min_qty(0.0001, 0.0));
    EXPECT_TRUE(meets_min_qty(0.001, 0.001));
    EXPECT_FALSE(meets_min_qty(0.0009, 0.001));
}

TEST(Instrument, MeetsMinNotional_ChecksProduct)
{
    // qty 0.001 * price 4999 = 4.999 < min 5 → fail
    EXPECT_FALSE(meets_min_notional(0.001, 4999.0, 5.0));
    // 0.001 * 5000 = 5 → pass
    EXPECT_TRUE (meets_min_notional(0.001, 5000.0, 5.0));
    // Unset min → always pass
    EXPECT_TRUE (meets_min_notional(0.0, 0.0, 0.0));
}
