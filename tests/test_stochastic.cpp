#include <gtest/gtest.h>
#include "indicator/stochastic.h"

#include <cmath>

TEST(Stochastic, WarmupPeriod_DefaultParams)
{
    stochastic_oscillator stoch; // (5,3,3)

    // First 4 bars: not enough for raw %K (needs 5)
    for (int i = 0; i < 4; ++i)
    {
        auto v = stoch.update(10.0 + i, 9.0 + i, 9.5 + i);
        EXPECT_FALSE(v.has_value());
        EXPECT_FALSE(stoch.ready());
    }

    // 5th bar → raw %K possible, but smoothing SMAs still warming
    auto v = stoch.update(14.0, 13.0, 13.5);
    EXPECT_FALSE(v.has_value());
    EXPECT_FALSE(stoch.ready());
}

TEST(Stochastic, BecomesReady_AfterSufficientBars)
{
    stochastic_oscillator stoch(5, 3, 3);

    // Feed 5 + 3 + 3 - 2 = 9 bars to get first %D (conservative)
    for (int i = 0; i < 9; ++i)
    {
        double h = 100.0 + i * 0.5;
        double l = 99.0 + i * 0.3;
        double c = 99.5 + i * 0.4;
        stoch.update(h, l, c);
    }

    // After enough bars it must become ready
    EXPECT_TRUE(stoch.ready());
    EXPECT_TRUE(std::isfinite(stoch.k()));
    EXPECT_TRUE(std::isfinite(stoch.d()));
}

TEST(Stochastic, ValuesInValidRange_0_to_100)
{
    stochastic_oscillator stoch(5, 3, 3);

    for (int i = 0; i < 20; ++i)
    {
        double h = 100.0 + std::sin(i * 0.7) * 5.0;
        double l = h - 2.0 - std::abs(std::sin(i * 1.3)) * 3.0;
        double c = (h + l) * 0.5 + std::cos(i * 0.9) * 1.5;
        stoch.update(h, l, c);
    }

    if (stoch.ready())
    {
        EXPECT_GE(stoch.k(), 0.0);
        EXPECT_LE(stoch.k(), 100.0);
        EXPECT_GE(stoch.d(), 0.0);
        EXPECT_LE(stoch.d(), 100.0);
        EXPECT_GE(stoch.raw_k(), 0.0);
        EXPECT_LE(stoch.raw_k(), 100.0);
    }
}

TEST(Stochastic, FlatRange_ProducesNeutralOrStableOutput)
{
    stochastic_oscillator stoch(5, 2, 2);

    // Completely flat market
    for (int i = 0; i < 15; ++i)
    {
        stoch.update(100.0, 100.0, 100.0);
    }

    if (stoch.ready())
    {
        // When high == low the implementation falls back to 50.0 for raw_k
        // After smoothing we should be very close to 50
        EXPECT_NEAR(stoch.k(), 50.0, 0.5);
        EXPECT_NEAR(stoch.d(), 50.0, 0.5);
    }
}

TEST(Stochastic, Reset_ClearsState)
{
    stochastic_oscillator stoch(5, 3, 3);

    for (int i = 0; i < 20; ++i)
    {
        stoch.update(100.0 + i, 99.0 + i * 0.5, 99.5 + i * 0.6);
    }
    ASSERT_TRUE(stoch.ready());

    double k_before = stoch.k();
    double d_before = stoch.d();

    stoch.reset();

    EXPECT_FALSE(stoch.ready());

    // After reset we need to warm up again.
    // With (5,3,3) we need roughly 5 (range) + 3 + 3 bars before ready().
    // Feed a clearly insufficient number first (6 bars is guaranteed not enough).
    for (int i = 0; i < 6; ++i)
    {
        stoch.update(100.0 + i, 99.0 + i * 0.5, 99.5 + i * 0.6);
    }
    EXPECT_FALSE(stoch.ready());

    // Feed the rest until we are ready again (same sequence as first run)
    for (int i = 7; i < 20; ++i)
    {
        stoch.update(100.0 + i, 99.0 + i * 0.5, 99.5 + i * 0.6);
    }

    EXPECT_TRUE(stoch.ready());
    // Values should be identical to the first run (same input sequence after reset)
    EXPECT_DOUBLE_EQ(stoch.k(), k_before);
    EXPECT_DOUBLE_EQ(stoch.d(), d_before);
}

TEST(Stochastic, CustomPeriods_Work)
{
    stochastic_oscillator stoch(3, 2, 2); // very short periods for fast readiness

    // Need at least 3 (k) + 2 (ksmooth) + 2 (d) - overlaps ≈ 6-7 bars
    for (int i = 0; i < 8; ++i)
    {
        stoch.update(10.0 + i, 9.0 + i * 0.5, 9.5 + i * 0.7);
    }

    EXPECT_TRUE(stoch.ready());
    EXPECT_GT(stoch.k_period(), 0u);
    EXPECT_GT(stoch.k_smoothing(), 0u);
    EXPECT_GT(stoch.d_period(), 0u);
}
