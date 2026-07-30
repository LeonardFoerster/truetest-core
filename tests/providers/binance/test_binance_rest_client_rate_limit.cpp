#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_rest_client.h"

TEST(BinanceRestRateLimit, BelowThresholdZeroDelay)
{
    // 50% weight use at mid-window -> no throttle.
    long long d = BinanceRestClient::throttle_delay_ms(
        /*used=*/3000,
        /*anchor=*/1'000'000,
        /*now=*/1'030'000,
        /*cap=*/6000,
        /*pct=*/80);
    EXPECT_EQ(d, 0);
}

TEST(BinanceRestRateLimit, AtThresholdMidWindowDelays)
{
    // 90% weight used, 20s into the 60s window -> expect ~40s remaining.
    long long d = BinanceRestClient::throttle_delay_ms(
        /*used=*/5400,
        /*anchor=*/1'000'000,
        /*now=*/1'020'000,
        /*cap=*/6000,
        /*pct=*/80);
    EXPECT_GT(d, 0);
    EXPECT_LE(d, 60'000);
    EXPECT_EQ(d, 40'000);
}

TEST(BinanceRestRateLimit, AboveThresholdStaleWindowZero)
{
    // Anchor is older than 60s - window rolled, no throttle.
    long long d = BinanceRestClient::throttle_delay_ms(
        /*used=*/6000,
        /*anchor=*/1'000'000,
        /*now=*/1'070'000,
        /*cap=*/6000,
        /*pct=*/80);
    EXPECT_EQ(d, 0);
}

TEST(BinanceRestRateLimit, CapZeroIsDefensiveNoThrottle)
{
    long long d = BinanceRestClient::throttle_delay_ms(
        /*used=*/10000,
        /*anchor=*/0,
        /*now=*/30'000,
        /*cap=*/0,
        /*pct=*/80);
    EXPECT_EQ(d, 0);
}

TEST(BinanceRestRateLimit, PctZeroNoThrottle)
{
    long long d = BinanceRestClient::throttle_delay_ms(
        /*used=*/6000,
        /*anchor=*/0,
        /*now=*/10'000,
        /*cap=*/6000,
        /*pct=*/0);
    EXPECT_EQ(d, 0);
}

TEST(BinanceRestRateLimit, NegativeDeltaTreatedAsRolled)
{
    // Clock went backwards (or uninitialised anchor in future) -> no throttle.
    long long d = BinanceRestClient::throttle_delay_ms(
        /*used=*/6000,
        /*anchor=*/2'000'000,
        /*now=*/1'000'000,
        /*cap=*/6000,
        /*pct=*/80);
    EXPECT_EQ(d, 0);
}

TEST(BinanceRestRateLimit, SettersDoNotThrow)
{
    BinanceRestClient c("", "");
    c.set_weight_cap(1000);
    c.set_soft_threshold_pct(50);
    EXPECT_EQ(c.last_used_weight(), 0);
}

#endif // HAS_BINANCE
