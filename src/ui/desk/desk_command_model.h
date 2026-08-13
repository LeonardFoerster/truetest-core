#pragma once

#include "ui/desk/desk_layout_model.h"

#include <array>
#include <string_view>

namespace truetest::ui::desk {

enum class DeskCommandKind : std::uint8_t
{
    select_page,
    reset_layout,
    toggle_demo,
    toggle_focus,
    toggle_layout_lock,
    toggle_density,
};

struct DeskCommand
{
    const char* label;
    const char* hint;
    DeskCommandKind kind;
    DeskPage page;
};

// Single-page desk for now (orderflow/liquidity/structure/markets/operations
// stay fully defined in desk_layout_model.h, just benched from desk_pages —
// trim their WORKSPACE commands here too so the palette can't "escape" to a
// dormant page and steal activity_blotter/health/risk from Monitor).
inline constexpr std::array<DeskCommand, 6> desk_commands = {{
    {"WORKSPACE MONITOR", "Positions, orders, activity, health and risk", DeskCommandKind::select_page, DeskPage::monitor},
    {"RESET LAYOUT", "Restore this workspace default", DeskCommandKind::reset_layout, DeskPage::count},
    {"TOGGLE DEMO DATA", "Enable or disable deterministic UI fixtures", DeskCommandKind::toggle_demo, DeskPage::count},
    {"FOCUS PRIMARY", "Maximize or restore the primary surface", DeskCommandKind::toggle_focus, DeskPage::count},
    {"TOGGLE LAYOUT LOCK", "Lock or unlock panel docking", DeskCommandKind::toggle_layout_lock, DeskPage::count},
    {"TOGGLE DENSITY", "Switch compact or comfortable table rows", DeskCommandKind::toggle_density, DeskPage::count},
}};

constexpr bool ascii_contains_case_insensitive(std::string_view text,
                                                std::string_view query) noexcept
{
    if (query.empty())
        return true;
    if (query.size() > text.size())
        return false;
    for (std::size_t start = 0; start + query.size() <= text.size(); ++start)
    {
        bool match = true;
        for (std::size_t i = 0; i < query.size(); ++i)
        {
            const auto lower = [](unsigned char value) constexpr {
                return value >= 'A' && value <= 'Z'
                    ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
            };
            const auto a = lower(static_cast<unsigned char>(text[start + i]));
            const auto b = lower(static_cast<unsigned char>(query[i]));
            if (a != b)
            {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

constexpr bool desk_command_matches(const DeskCommand& command,
                                    std::string_view query) noexcept
{
    return ascii_contains_case_insensitive(command.label, query)
        || ascii_contains_case_insensitive(command.hint, query);
}

constexpr bool operator_shortcut_allowed(bool ctrl,
                                         bool alt,
                                         bool super,
                                         bool shift,
                                         bool want_text,
                                         bool palette_open,
                                         bool confirm_open) noexcept
{
    return !ctrl && !alt && !super && !shift && !want_text
        && !palette_open && !confirm_open;
}

} // namespace truetest::ui::desk
