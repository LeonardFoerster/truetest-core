#include <gtest/gtest.h>
#include "execution/rate_limiter.h"

#include <thread>
#include <vector>

TEST(RateLimiter, FullBucketAllowsBurstUpToCapacity)
{
    TokenBucketRateLimiter rl(/*capacity=*/5, /*refill_per_sec=*/0.0);
    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(rl.try_acquire()) << "burst slot " << i;
    EXPECT_FALSE(rl.try_acquire()) << "6th try_acquire in a drained bucket must fail";
}

TEST(RateLimiter, TimeUntilReturnsZeroWhenAvailable)
{
    TokenBucketRateLimiter rl(10, 1.0);
    EXPECT_EQ(rl.time_until(1.0).count(), 0);
}

TEST(RateLimiter, TimeUntilGrowsWithDeficit)
{
    TokenBucketRateLimiter rl(1, 1.0);
    EXPECT_TRUE(rl.try_acquire(1));
    // Now zero tokens; to get 1 token at 1/sec needs ~1 second.
    auto wait = rl.time_until(1.0);
    EXPECT_GT(wait.count(), 900'000'000);    // > 0.9s
    EXPECT_LT(wait.count(), 1'100'000'000);  // < 1.1s
}

TEST(RateLimiter, RefillsOverTime)
{
    TokenBucketRateLimiter rl(/*capacity=*/2, /*refill_per_sec=*/100.0);
    EXPECT_TRUE(rl.try_acquire(2));
    EXPECT_FALSE(rl.try_acquire(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    // After ~30ms at 100/s we should have ~3 tokens accumulated, capped at 2.
    EXPECT_GT(rl.available_tokens(), 1.0);
}

TEST(RateLimiter, AcquireBlocking_EventuallySucceeds)
{
    TokenBucketRateLimiter rl(1, 50.0);
    EXPECT_TRUE(rl.try_acquire());
    auto start = std::chrono::steady_clock::now();
    rl.acquire_blocking(1);
    auto elapsed = std::chrono::steady_clock::now() - start;
    // Refill at 50/s means ~20ms to recover one token.
    EXPECT_GT(elapsed, std::chrono::milliseconds(10));
}

TEST(RateLimiter, ZeroRefillRate_StarvesAfterBurst)
{
    TokenBucketRateLimiter rl(1, 0.0);
    EXPECT_TRUE(rl.try_acquire());
    EXPECT_EQ(rl.time_until(1.0), std::chrono::nanoseconds::max());
}
