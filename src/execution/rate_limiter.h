#pragma once

// Token-bucket rate limiter for signed REST calls (Binance IP weight limits,
// order-rate caps, etc.). Thread-safe: callers can hit it from any worker.
//
// Capacity = burst size. refill_per_sec = sustained rate. `try_acquire` is the
// primary API — non-blocking; returns false if not enough tokens. Callers that
// want to wait should consult `time_until` and sleep themselves, or use
// `acquire_blocking` (which sleeps on the calling thread).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

class TokenBucketRateLimiter
{
public:
    using clock = std::chrono::steady_clock;

    TokenBucketRateLimiter(double capacity, double refill_per_sec)
        : capacity_(capacity),
          refill_per_sec_(refill_per_sec),
          tokens_(capacity),
          last_refill_(clock::now())
    {}

    bool try_acquire(double tokens = 1.0)
    {
        std::lock_guard<std::mutex> lk(mu_);
        refill_locked();
        if (tokens_ + 1e-9 >= tokens)
        {
            tokens_ -= tokens;
            return true;
        }
        return false;
    }

    std::chrono::nanoseconds time_until(double tokens = 1.0)
    {
        std::lock_guard<std::mutex> lk(mu_);
        refill_locked();
        if (tokens_ >= tokens) return std::chrono::nanoseconds::zero();
        if (refill_per_sec_ <= 0.0) return std::chrono::nanoseconds::max();
        double deficit = tokens - tokens_;
        double seconds = deficit / refill_per_sec_;
        return std::chrono::nanoseconds(
            static_cast<long long>(std::ceil(seconds * 1e9)));
    }

    void acquire_blocking(double tokens = 1.0)
    {
        while (!try_acquire(tokens))
        {
            auto wait = time_until(tokens);
            if (wait == std::chrono::nanoseconds::max()) return; // refill_per_sec_ == 0 → never
            std::this_thread::sleep_for(wait);
        }
    }

    double available_tokens()
    {
        std::lock_guard<std::mutex> lk(mu_);
        refill_locked();
        return tokens_;
    }

private:
    void refill_locked()
    {
        auto now = clock::now();
        auto elapsed = std::chrono::duration<double>(now - last_refill_).count();
        tokens_ = std::min(capacity_, tokens_ + elapsed * refill_per_sec_);
        last_refill_ = now;
    }

    std::mutex mu_;
    double capacity_;
    double refill_per_sec_;
    double tokens_;
    clock::time_point last_refill_;
};
