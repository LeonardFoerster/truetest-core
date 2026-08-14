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
#include <span>

namespace truetest::ui::desk::panels {

// One column of a begin_table() call. `flags`/`width` map straight onto
// ImGui::TableSetupColumn's own parameters - leave both at their default
// (0, 0.0f) for a stretch column with no explicit width.
struct TableColumn
{
    const char* label;
    ImGuiTableColumnFlags flags = 0;
    float width = 0.0f;
};

// Collapses the BeginTable + TableSetupColumn(...)*N [+ TableSetupScrollFreeze]
// + TableHeadersRow sequence hand-rolled at ~17 call sites across the desk
// panels. Caller still calls ImGui::EndTable() itself on success, matching
// existing call-site discipline (no RAII wrapper). freeze_cols/freeze_rows
// default to 0 (skip TableSetupScrollFreeze entirely) since most tables here
// don't freeze a header row/column.
inline bool begin_table(const char* id, std::span<const TableColumn> columns,
                        ImGuiTableFlags flags, ImVec2 size = ImVec2(0, -1),
                        int freeze_cols = 0, int freeze_rows = 0)
{
    if (!ImGui::BeginTable(id, static_cast<int>(columns.size()), flags, size))
        return false;
    for (const auto& col : columns)
        ImGui::TableSetupColumn(col.label, col.flags, col.width);
    if (freeze_cols > 0 || freeze_rows > 0)
        ImGui::TableSetupScrollFreeze(freeze_cols, freeze_rows);
    ImGui::TableHeadersRow();
    return true;
}

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
