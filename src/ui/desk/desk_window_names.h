#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace truetest::ui::desk {

// Stable window identifiers for the native desk. Visible titles may evolve,
// but these names are persisted by ImGui and therefore belong to layout v2.
enum class DeskPanel : std::uint8_t
{
    watchlist,
    orderflow_canvas,
    orderflow_dom,
    selected_context,
    activity_blotter,
    liquidity_heatmap,
    liquidity_dom,
    liquidations,
    liquidity_tape,
    tpo_profile,
    volume_profile,
    session_context,
    funding,
    correlation,
    market_detail,
    equity,
    operations_activity,
    strategies,
    risk,
    health,
    debug,
    // Legacy render entry points retained until their table helpers are fully
    // extracted. They are not launched by the v2 workspace model.
    positions,
    order_book,
    fills,
    lots,
    open_orders,
    market_chart,
    indicators,
    volume_flow,
    market_activity,
    signal_checklist,
    market_context,
    trade_history,
    count,
};

inline constexpr std::size_t desk_panel_count =
    static_cast<std::size_t>(DeskPanel::count);

inline constexpr std::array<const char*, desk_panel_count> desk_window_names = {
    "Watchlist##desk_v2",
    "Orderflow Canvas##desk_v2",
    "DOM##desk_v2",
    "Selected Context##desk_v2",
    "Activity Blotter##desk_v2",
    "Liquidity Heatmap##desk_v2",
    "Liquidity DOM##desk_v2",
    "Liquidations##desk_v2",
    "Liquidity Tape##desk_v2",
    "Market Profile / TPO##desk_v2",
    "Volume Profile##desk_v2",
    "Session Context##desk_v2",
    "Funding Intelligence##desk_v2",
    "Correlation##desk_v2",
    "Market Detail##desk_v2",
    "Equity & Drawdown##desk_v2",
    "Operations Activity##desk_v2",
    "Strategies##desk_v2",
    "Risk##desk_v2",
    "Health##desk_v2",
    "Debug##desk_v2",
    "Positions##legacy",
    "Order Book##legacy",
    "Fills##legacy",
    "Lots & Brackets##legacy",
    "Open Orders##legacy",
    "Market Chart##legacy",
    "Indicators##legacy",
    "Volume & Flow##legacy",
    "Market Activity##legacy",
    "Signal Checklist##legacy",
    "Market Context##legacy",
    "Trade History##legacy",
};

constexpr const char* desk_window_name(DeskPanel panel) noexcept
{
    return desk_window_names[static_cast<std::size_t>(panel)];
}

} // namespace truetest::ui::desk
