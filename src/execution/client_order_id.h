#pragma once

// Idempotent client-order-id minter for live submission.
//
// Each engine run gets a compact prefix derived from its start time + seed;
// within the run, IDs are a monotonic counter. This gives two properties:
//   1. Replays / re-submissions with identical (seed, wall-clock start) pair
//      produce identical IDs — exchange idempotency works.
//   2. Different runs can't collide even if seeds match, because wall-clock
//      differs.
// The Binance clientOrderId limit is 36 chars; this minter caps at ~30.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

class ClientOrderIdMinter
{
public:
    // Wall-clock-seeded. Two runs with the same seed but different wall-clock
    // start times produce different IDs.
    ClientOrderIdMinter(const std::string& run_prefix, std::uint64_t seed)
        : ClientOrderIdMinter(run_prefix, seed, now_epoch_ms()) {}

    // Deterministic form: same (prefix, seed, epoch_ms) → same ID sequence.
    // Used by replay to reproduce the exact IDs a prior run submitted.
    ClientOrderIdMinter(const std::string& run_prefix,
                        std::uint64_t seed,
                        std::int64_t epoch_ms)
        : seq_(0)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s-%llx-%llx-",
                      run_prefix.c_str(),
                      static_cast<unsigned long long>(epoch_ms),
                      static_cast<unsigned long long>(seed));
        prefix_ = buf;
    }

    std::string next()
    {
        std::uint64_t n = ++seq_;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%s%llx",
                      prefix_.c_str(), static_cast<unsigned long long>(n));
        return buf;
    }

    const std::string& prefix() const { return prefix_; }

private:
    static std::int64_t now_epoch_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string prefix_;
    std::atomic<std::uint64_t> seq_;
};
