#pragma once

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <ratio>
#include <string_view>
#include <type_traits>

// Strict UTC market-data timestamp parsing. Numeric epochs are accepted only
// in the two documented legacy forms (10 decimal digits = seconds, 13 =
// milliseconds). ISO civil timestamps require complete token consumption and
// use UTC when no suffix is present; Z and explicit +/-HH:MM offsets are
// supported. Epoch zero and values outside system_clock are rejected.
namespace tt::date_parse {

namespace detail {

inline std::string_view trim_ascii(std::string_view value) noexcept
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
        value.remove_suffix(1);
    return value;
}

inline bool all_digits(std::string_view value) noexcept
{
    if (value.empty()) return false;
    for (const char character : value) {
        if (character < '0' || character > '9') return false;
    }
    return true;
}

template <typename Integer>
inline bool parse_integer(std::string_view value, Integer& output) noexcept
{
    static_assert(std::is_integral_v<Integer>);
    if (value.empty()) return false;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), output);
    return error == std::errc{} && end == value.data() + value.size();
}

inline bool parse_fixed_int(std::string_view value, std::size_t offset, std::size_t length,
                            int& output) noexcept
{
    if (offset > value.size() || length > value.size() - offset) return false;
    const std::string_view component = value.substr(offset, length);
    return all_digits(component) && parse_integer(component, output);
}

inline bool is_leap_year(int year) noexcept
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

inline bool valid_civil_date(int year, int month, int day) noexcept
{
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1) return false;
    constexpr int days_by_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maximum_day = days_by_month[month - 1];
    if (month == 2 && is_leap_year(year)) ++maximum_day;
    return day <= maximum_day;
}

// Howard Hinnant's days-from-civil algorithm. Call only after validating the
// civil fields. The supported 4-digit year range cannot overflow int64_t.
inline std::int64_t days_from_civil(int year, unsigned month, unsigned day) noexcept
{
    year -= month <= 2U;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const auto year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned adjusted_month = month > 2U ? month - 3U : month + 9U;
    const unsigned day_of_year = (153U * adjusted_month + 2U) / 5U + day - 1U;
    const unsigned day_of_era =
        year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return static_cast<std::int64_t>(era) * 146097LL + static_cast<std::int64_t>(day_of_era) -
           719468LL;
}

template <typename SourcePeriod>
inline std::optional<std::chrono::system_clock::time_point>
checked_time_point(std::int64_t count) noexcept
{
    using target_duration = std::chrono::system_clock::duration;
    using target_rep = target_duration::rep;
    using conversion = std::ratio_divide<SourcePeriod, typename target_duration::period>;
    static_assert(std::is_integral_v<target_rep>);
    static_assert(conversion::den == 1, "system_clock ticks must exactly divide milliseconds");
    using checked_rep = std::common_type_t<std::int64_t, target_rep, std::intmax_t>;
    const checked_rep maximum = std::numeric_limits<target_rep>::max();
    const checked_rep multiplier = conversion::num;
    if (count <= 0 || static_cast<checked_rep>(count) > maximum / multiplier) return std::nullopt;
    const checked_rep converted = static_cast<checked_rep>(count) * multiplier;
    return std::chrono::system_clock::time_point{
        target_duration{static_cast<target_rep>(converted)}};
}

struct ParsedOffset
{
    bool valid = false;
    int seconds = 0;
};

inline ParsedOffset parse_utc_offset(std::string_view suffix) noexcept
{
    if (suffix.empty() || suffix == "Z") return {true, 0};
    if (suffix.size() != 6 || (suffix.front() != '+' && suffix.front() != '-') || suffix[3] != ':')
        return {};

    int hours = 0;
    int minutes = 0;
    if (!parse_fixed_int(suffix, 1, 2, hours) || !parse_fixed_int(suffix, 4, 2, minutes) ||
        hours > 14 || minutes > 59 || (hours == 14 && minutes != 0))
        return {};

    const int magnitude = hours * 3600 + minutes * 60;
    return {true, suffix.front() == '+' ? magnitude : -magnitude};
}

inline std::optional<std::chrono::system_clock::time_point>
parse_iso(std::string_view value) noexcept
{
    if (value.size() < 10 || value[4] != '-' || value[7] != '-') return std::nullopt;

    int year = 0;
    int month = 0;
    int day = 0;
    if (!parse_fixed_int(value, 0, 4, year) || !parse_fixed_int(value, 5, 2, month) ||
        !parse_fixed_int(value, 8, 2, day) || !valid_civil_date(year, month, day))
        return std::nullopt;

    int hour = 0;
    int minute = 0;
    int second = 0;
    int fractional_milliseconds = 0;
    ParsedOffset offset{true, 0};

    if (value.size() != 10) {
        if (value.size() < 16 || (value[10] != 'T' && value[10] != ' ') || value[13] != ':' ||
            !parse_fixed_int(value, 11, 2, hour) || !parse_fixed_int(value, 14, 2, minute))
            return std::nullopt;

        std::size_t cursor = 16;
        if (cursor < value.size() && value[cursor] == ':') {
            if (value.size() < 19 || !parse_fixed_int(value, 17, 2, second)) return std::nullopt;
            cursor = 19;
            if (cursor < value.size() && value[cursor] == '.') {
                const std::size_t digits_begin = ++cursor;
                while (cursor < value.size() && value[cursor] >= '0' && value[cursor] <= '9')
                    ++cursor;
                const std::size_t digit_count = cursor - digits_begin;
                if (digit_count == 0 || digit_count > 3 ||
                    !parse_fixed_int(value, digits_begin, digit_count, fractional_milliseconds))
                    return std::nullopt;
                if (digit_count == 1) fractional_milliseconds *= 100;
                if (digit_count == 2) fractional_milliseconds *= 10;
            }
        }

        if (hour > 23 || minute > 59 || second > 59) return std::nullopt;
        offset = parse_utc_offset(value.substr(cursor));
        if (!offset.valid) return std::nullopt;
    }

    const std::int64_t days =
        days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const std::int64_t local_seconds = days * 86400LL + static_cast<std::int64_t>(hour) * 3600LL +
                                       static_cast<std::int64_t>(minute) * 60LL + second;
    const std::int64_t utc_seconds = local_seconds - offset.seconds;
    const std::int64_t epoch_milliseconds = utc_seconds * 1000LL + fractional_milliseconds;
    if (epoch_milliseconds <= 0) return std::nullopt;
    return checked_time_point<std::milli>(epoch_milliseconds);
}

}  // namespace detail

inline std::optional<std::chrono::system_clock::time_point>
from_epoch_milliseconds(std::int64_t value) noexcept
{
    if (value <= 0) return std::nullopt;
    return detail::checked_time_point<std::milli>(value);
}

inline std::optional<std::chrono::system_clock::time_point>
from_epoch_seconds(std::int64_t value) noexcept
{
    if (value <= 0) return std::nullopt;
    return detail::checked_time_point<std::ratio<1>>(value);
}

inline std::optional<std::chrono::system_clock::time_point> parse(std::string_view input) noexcept
{
    const std::string_view value = detail::trim_ascii(input);
    if (value.empty()) return std::nullopt;

    if (detail::all_digits(value)) {
        std::int64_t epoch = 0;
        if ((value.size() != 10 && value.size() != 13) || !detail::parse_integer(value, epoch))
            return std::nullopt;
        return value.size() == 10 ? from_epoch_seconds(epoch) : from_epoch_milliseconds(epoch);
    }

    return detail::parse_iso(value);
}

// New callers that know whether an explicit numeric field was present should
// use this API. An invalid explicit open_time never falls back to the date.
inline std::optional<std::chrono::system_clock::time_point>
try_resolve_bar_clock(std::optional<std::int64_t> open_time_ms, std::string_view date) noexcept
{
    if (open_time_ms) return from_epoch_milliseconds(*open_time_ms);
    return parse(date);
}

// Compatibility path for frozen engine call sites. It preserves the existing
// last-known-time fallback but now prevents chrono overflow. Migration to
// try_resolve_bar_clock() is required before malformed-time handling is fully
// fail-closed.
inline std::chrono::system_clock::time_point
resolve_bar_clock(std::int64_t open_time_ms, std::string_view date,
                  std::chrono::system_clock::time_point fallback) noexcept
{
    if (open_time_ms > 0) {
        if (const auto timestamp = from_epoch_milliseconds(open_time_ms)) return *timestamp;
        return fallback;
    }
    if (const auto timestamp = parse(date)) return *timestamp;
    return fallback;
}

}  // namespace tt::date_parse
