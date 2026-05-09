#ifdef HAS_QUESTDB

#include "run_tag.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <stdexcept>

namespace truetest::questdb {

bool is_valid_run_tag(const std::string& tag)
{
    if (tag.empty() || tag.size() > 64) return false;
    for (char c : tag)
    {
        const bool ok = (c >= '0' && c <= '9')
                     || (c >= 'A' && c <= 'Z')
                     || (c >= 'a' && c <= 'z')
                     || c == '_';
        if (!ok) return false;
    }
    return true;
}

std::string make_run_tag(const std::string& user_override,
                         std::uint64_t test_seed)
{
    if (!user_override.empty())
    {
        if (!is_valid_run_tag(user_override))
        {
            throw std::invalid_argument(
                "run_tag must match [A-Za-z0-9_]{1,64}");
        }
        return user_override;
    }

    // Wall-clock components.
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif

    // 24-bit hex suffix.
    std::uint32_t suffix = 0;
    if (test_seed != 0)
    {
        std::mt19937_64 rng(test_seed);
        suffix = static_cast<std::uint32_t>(rng() & 0xFFFFFFu);
    }
    else
    {
        std::random_device rd;
        std::mt19937_64 rng(static_cast<std::uint64_t>(rd())
            ^ static_cast<std::uint64_t>(std::chrono::steady_clock::now()
                                         .time_since_epoch().count()));
        suffix = static_cast<std::uint32_t>(rng() & 0xFFFFFFu);
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "run_%04d%02d%02d_%02d%02d%02d_%06x",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  suffix);
    return std::string(buf);
}

} // namespace truetest::questdb

#endif // HAS_QUESTDB
