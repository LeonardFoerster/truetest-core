#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/desk/desk_theme.h"

#include "imgui.h"

namespace truetest::ui::desk::widgets {

inline void mono_text(const char* text, ImVec4 color = theme::text())
{
    const bool pushed = theme::push_mono_font();
    ImGui::TextColored(color, "%s", text);
    theme::pop_mono_font(pushed);
}

inline ImVec4 pnl_color(double value)
{
    return value > 0.0 ? theme::positive()
                       : (value < 0.0 ? theme::negative() : theme::text_muted());
}

}  // namespace truetest::ui::desk::widgets

#endif  // HAS_IMGUI_DESK
