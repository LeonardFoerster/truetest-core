#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

// Permissive bar-date parser. Used by the engine to promote CSV date columns
// from strings into real time_points. Handles:
//   - 13-digit numeric  → epoch milliseconds
//   - 10-digit numeric  → epoch seconds
//   - "YYYY-MM-DD"      → midnight UTC on that date
//   - "YYYY-MM-DD HH:MM:SS" or "YYYY-MM-DDTHH:MM:SS[.fff][Z]" → UTC instant
//
// Returns std::nullopt when the string is empty or none of the patterns match.
// Callers then fall back to synthetic stepping (bar_interval offsets from a
// base timestamp) so an unparseable date does not poison the whole run.
namespace tt::date_parse
{

namespace detail
{

inline bool all_digits(std::string_view s)
{
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// timegm(): convert a UTC std::tm to epoch seconds. Portable fallback,
// avoids pulling in nonstandard extensions on MSVC.
inline std::int64_t utc_tm_to_epoch_s(const std::tm& t)
{
    // Days from civil 1970-01-01 to the given year/month/day, per the
    // Howard Hinnant algorithm.
    int y = t.tm_year + 1900;
    int m = t.tm_mon + 1;
    int d = t.tm_mday;
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = era * 146097LL + static_cast<std::int64_t>(doe) - 719468LL;
    return days * 86400LL + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

} // namespace detail

inline std::optional<std::chrono::system_clock::time_point>
parse(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    if (s.empty()) return std::nullopt;

    using tp = std::chrono::system_clock::time_point;
    using ms = std::chrono::milliseconds;
    using sec = std::chrono::seconds;

    if (detail::all_digits(s))
    {
        std::int64_t v = 0;
        for (char c : s) v = v * 10 + (c - '0');
        if (s.size() >= 13) return tp{ms{v}};
        if (s.size() >= 10) return tp{sec{v}};
        return std::nullopt; // 8 digits like 20240101 would be ambiguous
    }

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0, frac_ms = 0;
    char sep = 0;
    // Buffer a null-terminated copy since sscanf doesn't take string_view.
    std::string buf(s);

    // YYYY-MM-DDTHH:MM:SS.fff[Z] or with space separator
    if (std::sscanf(buf.c_str(), "%4d-%2d-%2d%c%2d:%2d:%2d.%3d",
                    &y, &mo, &d, &sep, &h, &mi, &se, &frac_ms) >= 7 &&
        (sep == 'T' || sep == ' '))
    {
        std::tm t{};
        t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
        t.tm_hour = h; t.tm_min = mi; t.tm_sec = se;
        auto epoch_s = detail::utc_tm_to_epoch_s(t);
        return tp{ms{epoch_s * 1000 + frac_ms}};
    }
    if (std::sscanf(buf.c_str(), "%4d-%2d-%2d",
                    &y, &mo, &d) == 3)
    {
        std::tm t{};
        t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
        auto epoch_s = detail::utc_tm_to_epoch_s(t);
        return tp{sec{epoch_s}};
    }
    return std::nullopt;
}

} // namespace tt::date_parse
