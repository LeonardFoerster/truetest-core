#pragma once

#include <cstdint>
#include <string>

namespace truetest::ui::desk {

enum class DeskBlotterTab : std::uint8_t
{
    positions,
    orders,
    protection,
    fills,
};

enum class MarketWatchSort : std::uint8_t
{
    symbol,
    mark,
    spread_bps,
    position,
};

// UI-owned interaction state. It never escapes into the engine snapshot and
// therefore cannot alter valuation, order state, or any safety decision.
struct DeskState
{
    std::string selected_symbol;
    std::uint64_t selected_order_id = 0;
    DeskBlotterTab active_blotter = DeskBlotterTab::positions;
    MarketWatchSort market_sort = MarketWatchSort::symbol;
    bool market_sort_descending = false;
};

}  // namespace truetest::ui::desk
