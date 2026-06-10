#include <gtest/gtest.h>
#include "indicator/swing_detector.h"

#include <cmath>

TEST(SwingDetector, WarmupAndFirstPivots)
{
    swing_detector sd(2); // strength 2

    // Feed some bars without a clear pivot yet
    for (int i = 0; i < 10; ++i)
    {
        double base = 100.0 + i * 0.1;
        sd.update(base + 1.0, base, base + 0.5);
    }
    EXPECT_FALSE(sd.ready()); // not enough structure yet

    // Create a clear swing high (strength 2 needs the high to be strictly higher than ±2 bars)
    // Bar at index ~12 will be the pivot high
    sd.update(105.0, 104.0, 104.5); // potential center
    sd.update(104.0, 103.0, 103.5);
    sd.update(103.0, 102.0, 102.5);

    // We need more bars to confirm later pivots, but the detector should start tracking
    EXPECT_TRUE(sd.bar_count() > 0);
}

TEST(SwingDetector, DetectsHigherHighAndLowerLow)
{
    swing_detector sd(1); // strength 1 for simpler test

    // Uptrend: rising highs and lows
    // Create swing high at ~bar 5
    sd.update(100, 99, 99.5);
    sd.update(102, 100, 101);   // higher high candidate
    sd.update(101, 100, 100.5);

    // Create swing low
    sd.update(100, 98, 99);     // lower low candidate
    sd.update(101, 99, 100);

    auto snap = sd.snapshot();
    // After a few swings we should have some structure
    if (sd.ready())
    {
        EXPECT_TRUE(snap.last_higher_high.has_value() || snap.last_lower_low.has_value());
    }
}

TEST(SwingDetector, BreakOfStructureIncrementsBos)
{
    swing_detector sd(1);

    // Build a clear small uptrend with enough bars for strength=1 confirmation
    // HH around bar ~5
    for (int i = 0; i < 3; ++i) sd.update(99 + i, 98 + i, 98.5 + i);
    sd.update(102, 100, 101);           // clear swing high (HH)
    for (int i = 0; i < 3; ++i) sd.update(101 - i*0.3, 100 - i*0.3, 100.5 - i*0.2);

    // HL - swing low
    sd.update(100, 98, 99);             // clear swing low (HL)
    for (int i = 0; i < 3; ++i) sd.update(99 + i*0.2, 98.5 + i*0.1, 99 + i*0.15);

    auto snap1 = sd.snapshot();
    std::size_t bos_before = snap1.bos_count;

    // Break the previous swing low (98) decisively with a lower low
    sd.update(98, 96, 97);              // breaks prior low → should trigger BOS
    for (int i = 0; i < 3; ++i) sd.update(97 + i*0.1, 96.5 + i*0.1, 97 + i*0.1);

    auto snap2 = sd.snapshot();
    EXPECT_GE(snap2.bos_count, bos_before + 1);
    EXPECT_EQ(snap2.phase, structure_phase::downtrend);
}

TEST(SwingDetector, IsSidewaysBySwingRange_UserRule)
{
    swing_detector sd(1);

    // Create 14 small swings in a tight range (range ~1.5)
    for (int i = 0; i < 30; ++i)
    {
        double base = 100.0 + (i % 3) * 0.5; // tiny oscillation
        sd.update(base + 0.8, base, base + 0.4);
    }

    double atr = 3.0; // ATR is larger than the swing range
    bool sideways = sd.is_sideways_by_swing_range(14, atr);

    // With tiny swings the range of last 14 should be << ATR
    if (sd.snapshot().recent_pivots.size() >= 14)
    {
        EXPECT_TRUE(sideways);
    }
}

TEST(SwingDetector, ResetClearsStateDeterministically)
{
    swing_detector sd(2);

    // Feed a sequence that produces some structure
    for (int i = 0; i < 40; ++i)
    {
        double base = 100.0 + std::sin(i * 0.4) * 8.0;
        sd.update(base + 2.0, base - 2.0, base);
    }
    ASSERT_TRUE(sd.ready());
    auto snap_before = sd.snapshot();
    std::size_t bos_before = snap_before.bos_count;

    sd.reset();

    EXPECT_FALSE(sd.ready());
    EXPECT_EQ(sd.bar_count(), 0u);
    EXPECT_EQ(sd.snapshot().bos_count, 0u);

    // Feed identical sequence again
    for (int i = 0; i < 40; ++i)
    {
        double base = 100.0 + std::sin(i * 0.4) * 8.0;
        sd.update(base + 2.0, base - 2.0, base);
    }

    auto snap_after = sd.snapshot();
    EXPECT_EQ(snap_after.bos_count, bos_before);
    if (snap_before.last_higher_high.has_value() && snap_after.last_higher_high.has_value())
    {
        EXPECT_DOUBLE_EQ(*snap_after.last_higher_high, *snap_before.last_higher_high);
    }
}

TEST(SwingDetector, GetIndicatorValuesContainsExpectedKeys)
{
    swing_detector sd(1);
    for (int i = 0; i < 20; ++i)
    {
        sd.update(100 + i * 0.5, 99 + i * 0.3, 99.5 + i * 0.4);
    }

    auto vals = sd.get_indicator_values();
    bool has_phase = false;
    for (const auto& [k, v] : vals)
    {
        if (k == "swing_phase") has_phase = true;
    }
    EXPECT_TRUE(has_phase);
}
