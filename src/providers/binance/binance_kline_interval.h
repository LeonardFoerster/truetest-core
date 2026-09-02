#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace binance {

inline std::optional<std::int64_t> fixed_kline_interval_ms(
    std::string_view interval) noexcept
{
    using namespace std::literals;
    if (interval == "1s"sv) return 1'000;
    if (interval == "1m"sv) return 60'000;
    if (interval == "3m"sv) return 3 * 60'000;
    if (interval == "5m"sv) return 5 * 60'000;
    if (interval == "15m"sv) return 15 * 60'000;
    if (interval == "30m"sv) return 30 * 60'000;
    if (interval == "1h"sv) return 60 * 60'000;
    if (interval == "2h"sv) return 2 * 60 * 60'000;
    if (interval == "4h"sv) return 4 * 60 * 60'000;
    if (interval == "6h"sv) return 6 * 60 * 60'000;
    if (interval == "8h"sv) return 8 * 60 * 60'000;
    if (interval == "12h"sv) return 12 * 60 * 60'000;
    if (interval == "1d"sv) return 24 * 60 * 60'000;
    if (interval == "3d"sv) return 3 * 24 * 60 * 60'000;
    if (interval == "1w"sv) return 7 * 24 * 60 * 60'000;
    // Calendar-month candles have no fixed duration. The current record
    // contract cannot validate them without a calendar/time-zone model.
    return std::nullopt;
}

inline bool kline_times_match_fixed_interval(
    std::int64_t open_time_ms, std::int64_t close_time_ms,
    std::string_view interval) noexcept
{
    const auto duration = fixed_kline_interval_ms(interval);
    return duration && open_time_ms > 0 && close_time_ms >= open_time_ms
        && close_time_ms - open_time_ms == *duration - 1;
}

} // namespace binance
