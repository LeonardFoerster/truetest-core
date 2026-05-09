#pragma once

// Idempotent clientOrderId minter. Prefix = run_prefix + wall_clock + seed,
// id = prefix + counter. Same (seed, start) → same sequence (replay
// idempotency); different wall-clock → no cross-run collision. Capped at
// ~30 chars for Binance's 36-char limit.

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

class ClientOrderIdMinter
{
public:
    ClientOrderIdMinter(const std::string& run_prefix, std::uint64_t seed)
        : ClientOrderIdMinter(run_prefix, seed, now_epoch_ms()) {}

    // Deterministic form for replay.
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

    // Binance spot testnet's WAF rejects requests whose params contain
    // SQL keywords (OR, AND, SELECT, DROP, UNION, "--"). Caller checks
    // the live prefix before going hot so a misconfigured run fails at
    // startup instead of every order being silently 400'd by the venue.
    // Returns the offending token, or empty if safe. Case-insensitive.
    static std::string sql_keyword_in(const std::string& s)
    {
        static constexpr const char* forbidden[] = {
            "OR", "AND", "SELECT", "DROP", "UNION", "--"
        };
        std::string upper;
        upper.reserve(s.size());
        for (char c : s) upper.push_back(
            static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        for (const char* kw : forbidden)
            if (upper.find(kw) != std::string::npos)
                return kw;
        return {};
    }

private:
    static std::int64_t now_epoch_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string prefix_;
    std::atomic<std::uint64_t> seq_;
};
