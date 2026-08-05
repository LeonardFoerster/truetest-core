#pragma once

#include <cstdint>
#include <string>

namespace truetest::ui::desk {

enum class DeskDataState : std::uint8_t
{
    unavailable,
    snapshot,
    demo,
    live,
    stale,
    error,
};

enum class DeskDensity : std::uint8_t
{
    compact,
    comfortable,
};

struct DeskLinkContext
{
    std::string symbol = "BTCUSDT";
    std::string venue = "BINANCE";
    std::string interval = "1m";
    std::string session = "UTC";
    std::uint8_t link_group = 0;
    std::int64_t time_min_ms = 0;
    std::int64_t time_max_ms = 0;
    double price_min = 0.0;
    double price_max = 0.0;
    bool camera_initialized = false;
};

constexpr const char* desk_data_state_text(DeskDataState state) noexcept
{
    switch (state)
    {
    case DeskDataState::unavailable: return "UNAVAILABLE";
    case DeskDataState::snapshot:    return "ENGINE SNAPSHOT";
    case DeskDataState::demo:        return "DEMO DATA";
    case DeskDataState::live:        return "LIVE";
    case DeskDataState::stale:       return "STALE";
    case DeskDataState::error:       return "ERROR";
    }
    return "UNKNOWN";
}

constexpr float desk_row_height(DeskDensity density) noexcept
{
    return density == DeskDensity::compact ? 24.0f : 30.0f;
}

} // namespace truetest::ui::desk
