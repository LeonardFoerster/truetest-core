#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/desk/desk_context.h"

#include "imgui.h"

#include <cstring>

// Calm workstation palette. Color is reserved for hierarchy and state; panel
// surfaces stay neutral so semantic colors remain immediately recognizable.
namespace truetest::ui::desk::theme {

inline ImVec4 rgba(float r, float g, float b, float a = 1.0f)
{
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

inline float& ui_scale_slot()
{
    static float scale = 1.0f;
    return scale;
}

inline void set_ui_scale(float scale)
{
    ui_scale_slot() = scale > 0.0f ? scale : 1.0f;
}

inline float dp(float value)
{
    return value * ui_scale_slot();
}

// Surfaces
inline ImVec4 bg0()     { return rgba(11, 15, 20); }       // app
inline ImVec4 bg1()     { return rgba(16, 22, 29); }       // panel
inline ImVec4 bg2()     { return rgba(23, 32, 42); }       // elevated
inline ImVec4 bg3()     { return rgba(31, 43, 55); }       // hover
inline ImVec4 line()    { return ImVec4(1, 1, 1, 0.08f); }
inline ImVec4 line_hi() { return ImVec4(1, 1, 1, 0.14f); }

// Text
inline ImVec4 tx_hi()   { return rgba(230, 237, 243); }
inline ImVec4 tx_mid()  { return rgba(168, 179, 193); }
inline ImVec4 tx_lo()   { return rgba(111, 126, 142); }
inline ImVec4 tx_faint(){ return rgba(75, 89, 104); }

// Hierarchy / charts
inline ImVec4 accent()      { return rgba(224, 160, 59); }
inline ImVec4 accent_dim()  { return rgba(224, 160, 59, 0.18f); }
inline ImVec4 secondary()   { return rgba(77, 182, 229); }
inline ImVec4 secondary_dim(){ return rgba(77, 182, 229, 0.16f); }
inline ImVec4 info()        { return rgba(78, 169, 224); }
inline ImVec4 info_dim()    { return rgba(78, 169, 224, 0.17f); }
inline ImVec4 data_link()   { return rgba(77, 182, 229); }

// Semantic
inline ImVec4 up()       { return rgba(67, 194, 132); }
inline ImVec4 up_dim()   { return rgba(67, 194, 132, 0.15f); }
inline ImVec4 down()     { return rgba(231, 96, 103); }
inline ImVec4 down_dim() { return rgba(231, 96, 103, 0.16f); }
inline ImVec4 warn()     { return rgba(230, 169, 74); }
inline ImVec4 warn_dim() { return rgba(230, 169, 74, 0.17f); }
inline ImVec4 danger()   { return rgba(239, 72, 85); }
inline ImVec4 danger_dim(){ return rgba(239, 72, 85, 0.21f); }

enum class StatusTone
{
    neutral,
    info,
    positive,
    warning,
    negative,
    halted,
};

inline ImVec4 status_color(StatusTone tone)
{
    switch (tone)
    {
    case StatusTone::info:     return info();
    case StatusTone::positive: return up();
    case StatusTone::warning:  return warn();
    case StatusTone::negative: return down();
    case StatusTone::halted:   return danger();
    case StatusTone::neutral:  return tx_mid();
    }
    return tx_mid();
}

inline ImVec4 status_background(StatusTone tone)
{
    switch (tone)
    {
    case StatusTone::info:     return info_dim();
    case StatusTone::positive: return up_dim();
    case StatusTone::warning:  return warn_dim();
    case StatusTone::negative: return down_dim();
    case StatusTone::halted:   return danger_dim();
    case StatusTone::neutral:  return bg2();
    }
    return bg2();
}

inline ImVec4 pnl_color(double pnl)
{
    return pnl > 0.0 ? up() : (pnl < 0.0 ? down() : tx_mid());
}

inline ImVec4 mode_color(const char* mode)
{
    if (!mode) return tx_mid();
    if (std::strcmp(mode, "engine_live") == 0 || std::strcmp(mode, "live") == 0)
        return danger();
    if (std::strcmp(mode, "engine_shadow") == 0 || std::strcmp(mode, "shadow") == 0)
        return info();
    return accent();
}

inline ImVec4 health_color(bool healthy, bool warning = false)
{
    return warning ? warn() : (healthy ? up() : down());
}

inline ImU32 u32(ImVec4 c)
{
    return ImGui::ColorConvertFloat4ToU32(c);
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
    if (ImFont* font = mono_font_slot())
    {
        ImGui::PushFont(font);
        return true;
    }
    return false;
}

inline void pop_mono_font(bool pushed)
{
    if (pushed)
        ImGui::PopFont();
}

inline void apply()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 4.0f;
    s.ChildRounding     = 3.0f;
    s.FrameRounding     = 3.0f;
    s.PopupRounding     = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.GrabRounding      = 2.0f;
    s.TabRounding       = 3.0f;
    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.WindowPadding     = ImVec2(10, 10);
    s.FramePadding      = ImVec2(8, 5);
    s.ItemSpacing       = ImVec2(8, 6);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.ScrollbarSize     = 10.0f;
    s.GrabMinSize       = 8.0f;
    s.IndentSpacing     = 14.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = tx_hi();
    c[ImGuiCol_TextDisabled]          = tx_lo();
    c[ImGuiCol_WindowBg]              = bg0();
    c[ImGuiCol_ChildBg]               = bg1();
    c[ImGuiCol_PopupBg]               = bg2();
    c[ImGuiCol_Border]                = line();
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]               = bg2();
    c[ImGuiCol_FrameBgHovered]        = bg3();
    c[ImGuiCol_FrameBgActive]         = bg3();
    c[ImGuiCol_TitleBg]               = bg0();
    c[ImGuiCol_TitleBgActive]         = bg1();
    c[ImGuiCol_TitleBgCollapsed]      = bg0();
    c[ImGuiCol_MenuBarBg]             = bg1();
    c[ImGuiCol_ScrollbarBg]           = bg0();
    c[ImGuiCol_ScrollbarGrab]         = bg3();
    c[ImGuiCol_ScrollbarGrabHovered]  = line_hi();
    c[ImGuiCol_ScrollbarGrabActive]   = accent();
    c[ImGuiCol_CheckMark]             = accent();
    c[ImGuiCol_SliderGrab]            = accent();
    c[ImGuiCol_SliderGrabActive]      = accent();
    c[ImGuiCol_Button]                = bg2();
    c[ImGuiCol_ButtonHovered]         = bg3();
    c[ImGuiCol_ButtonActive]          = accent_dim();
    c[ImGuiCol_Header]                = bg2();
    c[ImGuiCol_HeaderHovered]         = bg3();
    c[ImGuiCol_HeaderActive]          = accent_dim();
    c[ImGuiCol_Separator]             = line();
    c[ImGuiCol_SeparatorHovered]      = accent();
    c[ImGuiCol_SeparatorActive]       = accent();
    c[ImGuiCol_ResizeGrip]            = line();
    c[ImGuiCol_ResizeGripHovered]     = accent();
    c[ImGuiCol_ResizeGripActive]      = accent();
    c[ImGuiCol_Tab]                   = bg1();
    c[ImGuiCol_TabHovered]            = bg3();
    c[ImGuiCol_TabActive]             = bg2();
    c[ImGuiCol_TabUnfocused]          = bg0();
    c[ImGuiCol_TabUnfocusedActive]    = bg1();
    c[ImGuiCol_DockingPreview]        = accent_dim();
    c[ImGuiCol_DockingEmptyBg]        = bg0();
    c[ImGuiCol_PlotLines]             = accent();
    c[ImGuiCol_PlotLinesHovered]      = up();
    c[ImGuiCol_PlotHistogram]         = accent();
    c[ImGuiCol_PlotHistogramHovered]  = up();
    c[ImGuiCol_TableHeaderBg]         = bg2();
    c[ImGuiCol_TableBorderStrong]     = line_hi();
    c[ImGuiCol_TableBorderLight]      = line();
    c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1, 1, 1, 0.02f);
    c[ImGuiCol_TextSelectedBg]        = accent_dim();
    c[ImGuiCol_DragDropTarget]        = accent();
    c[ImGuiCol_NavHighlight]          = accent();
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.55f);
}

// Panel chrome: title strip + body. Returns true if body should draw.
inline bool begin_panel(const char* title, const char* sub = nullptr,
                        ImVec2 size = ImVec2(0, 0), bool border = true)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg1());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    const bool open = ImGui::BeginChild(title, size, border,
                                        ImGuiWindowFlags_None);
    if (open)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, tx_mid());
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        if (sub && sub[0])
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, tx_faint());
            ImGui::TextUnformatted(sub);
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
        ImGui::Spacing();
    }
    return open;
}

inline void end_panel()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

inline void badge(const char* text, ImVec4 fg, ImVec4 bg)
{
    const ImVec2 pad(8, 3);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = 4.0f;
    dl->AddRectFilled(p, ImVec2(p.x + ts.x + pad.x * 2, p.y + ts.y + pad.y * 2),
                      u32(bg), r);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), u32(fg), text);
    ImGui::Dummy(ImVec2(ts.x + pad.x * 2, ts.y + pad.y * 2));
}

inline void status_badge(const char* text, StatusTone tone)
{
    badge(text, status_color(tone), status_background(tone));
}

inline void section_header(const char* label, const char* context = nullptr,
                           ImVec4 color = accent())
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        p, ImVec2(p.x + 3.0f, p.y + ImGui::GetTextLineHeight()), u32(color), 1.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 9.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, tx_mid());
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (context && context[0])
    {
        ImGui::SameLine();
        ImGui::TextColored(tx_faint(), "%s", context);
    }
}

inline void metric_card(const char* id, const char* label, const char* value,
                        const char* context, ImVec4 color, ImVec2 size)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg1());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 1));
    ImGui::BeginChild(id, size, true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 win = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(win.x + 1.0f, win.y + 1.0f),
        ImVec2(win.x + 4.0f, win.y + size.y - 1.0f), u32(color), 1.0f);
    ImGui::TextColored(tx_lo(), "%s", label);
    ImGui::SetWindowFontScale(1.12f);
    ImGui::TextColored(color, "%s", value);
    ImGui::SetWindowFontScale(1.0f);
    if (context && context[0])
        ImGui::TextColored(tx_faint(), "%s", context);
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

inline void danger_button_style(bool on)
{
    if (!on) return;
    ImGui::PushStyleColor(ImGuiCol_Button, danger_dim());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, danger());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, danger());
    ImGui::PushStyleColor(ImGuiCol_Text, tx_hi());
}

inline void danger_button_style_pop(bool on)
{
    if (on) ImGui::PopStyleColor(4);
}

inline StatusTone data_state_tone(DeskDataState state)
{
    switch (state)
    {
    case DeskDataState::demo:        return StatusTone::warning;
    case DeskDataState::snapshot:    return StatusTone::info;
    case DeskDataState::live:        return StatusTone::positive;
    case DeskDataState::stale:       return StatusTone::warning;
    case DeskDataState::error:       return StatusTone::negative;
    case DeskDataState::unavailable: return StatusTone::neutral;
    }
    return StatusTone::neutral;
}

inline void panel_meta(const char* title,
                       const char* context,
                       DeskDataState state,
                       std::int64_t age_ms)
{
    section_header(title, context, state == DeskDataState::demo ? warn() : accent());
    status_badge(desk_data_state_text(state), data_state_tone(state));
    if (age_ms > 0)
    {
        ImGui::SameLine();
        ImGui::TextColored(tx_faint(), "age %.1fs", static_cast<double>(age_ms) / 1000.0);
    }
    ImGui::Separator();
}

} // namespace truetest::ui::desk::theme

#endif // HAS_IMGUI_DESK
