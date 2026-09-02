#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/desk/desk_theme.h"

#include "imgui.h"

namespace truetest::ui::desk::widgets {

inline void status_badge(const char* label, theme::Tone tone)
{
    const ImVec2 padding{6.0f, 2.0f};
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 end{start.x + text_size.x + padding.x * 2.0f,
                     start.y + text_size.y + padding.y * 2.0f};
    ImGui::GetWindowDrawList()->AddRectFilled(start, end, theme::u32(theme::background(tone)),
                                              2.0f);
    ImGui::GetWindowDrawList()->AddText({start.x + padding.x, start.y + padding.y},
                                        theme::u32(theme::color(tone)), label);
    ImGui::Dummy({end.x - start.x, end.y - start.y});
}

inline void unavailable_text(const char* detail = "Unavailable")
{
    ImGui::TextColored(theme::text_faint(), "%s", detail);
}

}  // namespace truetest::ui::desk::widgets

#endif  // HAS_IMGUI_DESK
