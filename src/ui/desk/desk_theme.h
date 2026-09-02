#pragma once

#ifdef HAS_IMGUI_DESK

#include "imgui.h"

namespace truetest::ui::desk::theme {

enum class Tone
{
    neutral,
    accent,
    positive,
    negative,
    warning,
    danger,
    muted
};

inline ImVec4 rgba(float red, float green, float blue, float alpha = 1.0f)
{
    return {red / 255.0f, green / 255.0f, blue / 255.0f, alpha};
}

inline ImVec4 background()
{
    return rgba(10, 14, 18);
}
inline ImVec4 surface()
{
    return rgba(17, 23, 30);
}
inline ImVec4 surface_raised()
{
    return rgba(23, 31, 40);
}
inline ImVec4 surface_hover()
{
    return rgba(31, 42, 54);
}
inline ImVec4 border()
{
    return rgba(112, 132, 150, 0.22f);
}
inline ImVec4 text()
{
    return rgba(226, 232, 238);
}
inline ImVec4 text_muted()
{
    return rgba(148, 162, 175);
}
inline ImVec4 text_faint()
{
    return rgba(91, 107, 121);
}
inline ImVec4 accent()
{
    return rgba(73, 180, 215);
}
inline ImVec4 accent_dim()
{
    return rgba(73, 180, 215, 0.16f);
}
inline ImVec4 positive()
{
    return rgba(51, 190, 122);
}
inline ImVec4 negative()
{
    return rgba(231, 83, 88);
}
inline ImVec4 warning()
{
    return rgba(218, 164, 72);
}
inline ImVec4 danger()
{
    return rgba(244, 78, 86);
}

inline ImVec4 color(Tone tone)
{
    switch (tone) {
    case Tone::accent:
        return accent();
    case Tone::positive:
        return positive();
    case Tone::negative:
        return negative();
    case Tone::warning:
        return warning();
    case Tone::danger:
        return danger();
    case Tone::muted:
        return text_muted();
    case Tone::neutral:
        return text();
    }
    return text();
}

inline ImVec4 background(Tone tone)
{
    const ImVec4 foreground = color(tone);
    return {foreground.x, foreground.y, foreground.z, tone == Tone::neutral ? 0.10f : 0.16f};
}

inline ImU32 u32(ImVec4 color)
{
    return ImGui::ColorConvertFloat4ToU32(color);
}

inline ImFont*& mono_font_slot()
{
    static ImFont* font = nullptr;
    return font;
}

inline void set_mono_font(ImFont* font)
{
    mono_font_slot() = font;
}
inline bool push_mono_font()
{
    if (ImFont* font = mono_font_slot()) {
        ImGui::PushFont(font);
        return true;
    }
    return false;
}
inline void pop_mono_font(bool pushed)
{
    if (pushed) ImGui::PopFont();
}

inline void apply()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 2.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 2.0f;
    style.ScrollbarRounding = 2.0f;
    style.TabRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = {8.0f, 7.0f};
    style.FramePadding = {7.0f, 4.0f};
    style.ItemSpacing = {7.0f, 5.0f};
    style.ItemInnerSpacing = {6.0f, 4.0f};
    style.ScrollbarSize = 10.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = text();
    colors[ImGuiCol_TextDisabled] = text_faint();
    colors[ImGuiCol_WindowBg] = background();
    colors[ImGuiCol_ChildBg] = surface();
    colors[ImGuiCol_PopupBg] = surface_raised();
    colors[ImGuiCol_Border] = border();
    colors[ImGuiCol_FrameBg] = surface_raised();
    colors[ImGuiCol_FrameBgHovered] = surface_hover();
    colors[ImGuiCol_FrameBgActive] = surface_hover();
    colors[ImGuiCol_TitleBg] = surface();
    colors[ImGuiCol_TitleBgActive] = surface();
    colors[ImGuiCol_MenuBarBg] = surface();
    colors[ImGuiCol_ScrollbarBg] = background();
    colors[ImGuiCol_ScrollbarGrab] = surface_hover();
    colors[ImGuiCol_ScrollbarGrabHovered] = accent_dim();
    colors[ImGuiCol_ScrollbarGrabActive] = accent();
    colors[ImGuiCol_CheckMark] = accent();
    colors[ImGuiCol_SliderGrab] = accent();
    colors[ImGuiCol_SliderGrabActive] = accent();
    colors[ImGuiCol_Button] = surface_raised();
    colors[ImGuiCol_ButtonHovered] = surface_hover();
    colors[ImGuiCol_ButtonActive] = accent_dim();
    colors[ImGuiCol_Header] = accent_dim();
    colors[ImGuiCol_HeaderHovered] = surface_hover();
    colors[ImGuiCol_HeaderActive] = accent_dim();
    colors[ImGuiCol_Separator] = border();
    colors[ImGuiCol_SeparatorHovered] = accent();
    colors[ImGuiCol_SeparatorActive] = accent();
    colors[ImGuiCol_Tab] = surface();
    colors[ImGuiCol_TabHovered] = surface_hover();
    colors[ImGuiCol_TabActive] = surface_raised();
    colors[ImGuiCol_TableHeaderBg] = surface_raised();
    colors[ImGuiCol_TableBorderStrong] = border();
    colors[ImGuiCol_TableBorderLight] = border();
    colors[ImGuiCol_TableRowBgAlt] = {1.0f, 1.0f, 1.0f, 0.018f};
    colors[ImGuiCol_TextSelectedBg] = accent_dim();
    colors[ImGuiCol_NavHighlight] = accent();
    colors[ImGuiCol_ModalWindowDimBg] = {0.0f, 0.0f, 0.0f, 0.60f};
}

}  // namespace truetest::ui::desk::theme

#endif  // HAS_IMGUI_DESK
