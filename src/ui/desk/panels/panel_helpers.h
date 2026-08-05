#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/console_dashboard.h"
#include "ui/desk/desk_theme.h"
#include "ui/desk/format_scale.h"

#include "imgui.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

namespace truetest::ui::desk::panels {

inline void text_pnl(double value)
{
    const bool mono = theme::push_mono_font();
    ImGui::TextColored(theme::pnl_color(value),
                       "%s", fmt_signed_usd(value).c_str());
    theme::pop_mono_font(mono);
}

inline void text_side(char side)
{
    const bool longish = side_is_buy_or_long(side);
    ImGui::TextColored(longish ? theme::up() : theme::down(), "%s", side_word(side));
}

inline void text_side_qty(double qty)
{
    ImGui::TextColored(qty > 0.0 ? theme::up()
                                 : (qty < 0.0 ? theme::down() : theme::tx_mid()),
                       "%s %s", position_side(qty), fmt_qty(abs_qty(qty)).c_str());
}

inline void side_badge(char side)
{
    theme::status_badge(side_word(side),
                        side_is_buy_or_long(side) ? theme::StatusTone::positive
                                                  : theme::StatusTone::negative);
}

inline void position_side_badge(double qty)
{
    const auto tone = qty > 0.0 ? theme::StatusTone::positive
        : (qty < 0.0 ? theme::StatusTone::negative : theme::StatusTone::neutral);
    theme::status_badge(position_side(qty), tone);
}

inline void position_side_badge(char side)
{
    const bool is_long = side == 'L' || side == 'l';
    const bool is_short = side == 'S' || side == 's';
    theme::status_badge(is_long ? "LONG" : (is_short ? "SHORT" : "?"),
                        is_long ? theme::StatusTone::positive
                                : (is_short ? theme::StatusTone::negative
                                            : theme::StatusTone::neutral));
}

inline void order_status_badge(const char* status)
{
    const char* text = status && status[0] ? status : "N/A";
    auto tone = theme::StatusTone::neutral;
    if (std::strstr(text, "reject") || std::strstr(text, "error")
        || std::strstr(text, "fail"))
        tone = theme::StatusTone::negative;
    else if (std::strstr(text, "cancel"))
        tone = theme::StatusTone::warning;
    else if (std::strstr(text, "fill") || std::strstr(text, "done"))
        tone = theme::StatusTone::positive;
    else if (std::strstr(text, "open") || std::strstr(text, "new")
             || std::strstr(text, "work") || std::strstr(text, "partial"))
        tone = theme::StatusTone::info;
    theme::status_badge(text, tone);
}

inline void mono_text(const char* text)
{
    const bool mono = theme::push_mono_font();
    ImGui::TextUnformatted(text);
    theme::pop_mono_font(mono);
}

inline void section_label(const char* label)
{
    theme::section_header(label);
}

inline const char* provider_lifecycle_text(int state)
{
    switch (state)
    {
    case 0: return "closed";
    case 1: return "opening";
    case 2: return "open";
    case 3: return "error";
    default: return "unknown";
    }
}

inline const char* stream_state_text(connection_state state)
{
    switch (state)
    {
    case connection_state::idle:         return "idle";
    case connection_state::backfill:     return "backfill";
    case connection_state::waiting:      return "waiting";
    case connection_state::live:         return "live";
    case connection_state::reconnecting: return "reconnecting";
    case connection_state::halted:       return "halted";
    case connection_state::closed:       return "closed";
    }
    return "unknown";
}

inline int clipper_count(std::size_t size)
{
    constexpr auto max_count = static_cast<std::size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(size, max_count));
}

} // namespace truetest::ui::desk::panels

#endif // HAS_IMGUI_DESK
