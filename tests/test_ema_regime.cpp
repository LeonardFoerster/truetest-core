#include <gtest/gtest.h>
#include "indicator/ema_regime.h"
#include "indicator/swing_detector.h"
#include "indicator/atr.h"

TEST(EMARegime, BasicUpdateAndRegimeFlags)
{
    ema_regime_detector regime;

    // Start with EMAs close together (contracted)
    for (int i = 0; i < 20; ++i)
    {
        double price = 100.0 + i * 0.1;
        regime.update(price + 0.3, price);  // very small distance
    }

    EXPECT_TRUE(regime.ready());
    EXPECT_TRUE(regime.is_contracted() || regime.current_regime() == ema_regime::contracted);
}

TEST(EMARegime, YFormExpansionAfterContraction)
{
    ema_regime_detector regime(14, 40, 1.6, 3.0, 2.0);

    // Phase 1: contracted
    for (int i = 0; i < 30; ++i)
    {
        regime.update(100.0 + i * 0.05, 100.0 + i * 0.04);
    }

    // Phase 2: sudden expansion (simulating Y-form after sideways/contraction)
    for (int i = 30; i < 50; ++i)
    {
        regime.update(100.0 + i * 0.8, 100.0 + i * 0.1);  // fast separation
    }

    if (regime.ready())
    {
        // Should detect expansion
        EXPECT_TRUE(regime.is_y_form_expanding() || regime.current_regime() == ema_regime::y_expanding);
    }
}

TEST(EMARegime, SidewaysViaSwingRange_UserRule)
{
    ema_regime_detector regime;

    // Simulate sideways: very small swing range, decent ATR
    double swing_range = 1.2;
    double atr = 4.0;

    for (int i = 0; i < 20; ++i)
    {
        regime.update(100.0, 100.0, swing_range, atr);
    }

    EXPECT_TRUE(regime.is_sideways());
    EXPECT_EQ(regime.current_regime(), ema_regime::sideways);
}

TEST(EMARegime, SwingDetectorOverloadPassesActualRange)
{
    swing_detector swings(/*strength=*/1, /*max_history=*/32);
    average_true_range atr(/*period=*/3);
    for (int i = 0; i < 36; ++i)
    {
        const double base = 100.0 + (i % 3) * 0.5;
        swings.update(base + 0.8, base, base + 0.4);
        (void)atr.update(base + 3.0, base - 3.0, base);
    }
    ASSERT_TRUE(swings.ready());
    ASSERT_TRUE(atr.ready());
    ASSERT_GT(swings.recent_swing_range(14), 0.0);
    ASSERT_LT(swings.recent_swing_range(14), atr.value());

    ema_regime_detector regime;
    regime.update(/*ema_fast=*/102.0, /*ema_slow=*/100.0, swings, atr);

    EXPECT_TRUE(regime.is_sideways())
        << "the convenience overload must not replace a true range with zero";
    EXPECT_EQ(regime.current_regime(), ema_regime::sideways);
}

TEST(EMARegime, WideCautionFilter)
{
    ema_regime_detector regime;

    // Very wide EMAs
    regime.update(120.0, 100.0, 5.0, 3.0);  // 20% distance

    EXPECT_TRUE(regime.is_wide());
    EXPECT_EQ(regime.current_regime(), ema_regime::wide);
}

TEST(EMARegime, ResetClearsState)
{
    ema_regime_detector regime;

    for (int i = 0; i < 30; ++i)
    {
        regime.update(100.0 + i, 99.0 + i * 0.5, 2.0, 4.0);
    }
    ASSERT_TRUE(regime.ready());

    regime.reset();

    EXPECT_FALSE(regime.ready());
    EXPECT_FALSE(regime.is_sideways());
    EXPECT_FALSE(regime.is_y_form_expanding());
    EXPECT_FALSE(regime.is_wide());
}

TEST(EMARegime, GetIndicatorValuesContainsKeys)
{
    ema_regime_detector regime;
    regime.update(105.0, 100.0);

    auto vals = regime.get_indicator_values();
    bool has_regime = false;
    for (const auto& [k, v] : vals)
    {
        if (k == "ema_regime") has_regime = true;
    }
    EXPECT_TRUE(has_regime);
}
