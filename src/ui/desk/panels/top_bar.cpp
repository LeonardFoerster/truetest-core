#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/top_bar.h"

#include "ui/desk/desk_theme.h"
#include "ui/desk/widgets/status_badge.h"

#include "imgui.h"

#include <cstring>

namespace truetest::ui::desk::panels {
namespace {

bool is_live_target(const char* target)
{
    return target && (std::strcmp(target, "engine_live") == 0 || std::strcmp(target, "live") == 0);
}

const char* target_label(const char* target)
{
    if (!target) return "WAITING";
    if (std::strcmp(target, "engine_backtest") == 0 || std::strcmp(target, "backtest") == 0)
        return "BACKTEST";
    if (std::strcmp(target, "engine_shadow") == 0 || std::strcmp(target, "shadow") == 0)
        return "SHADOW";
    if (is_live_target(target)) return "LIVE";
    return "TARGET UNKNOWN";
}

const char* provider_state(int state)
{
    switch (state) {
    case 0:
        return "CLOSED";
    case 1:
        return "OPENING";
    case 2:
        return "CONNECTED";
    case 3:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

theme::Tone provider_tone(int state)
{
    switch (state) {
    case 2:
        return theme::Tone::accent;
    case 1:
        return theme::Tone::warning;
    case 3:
        return theme::Tone::danger;
    default:
        return theme::Tone::muted;
    }
}

void disabled_button(const char* label, bool enabled, bool danger, bool& pressed)
{
    if (!enabled) ImGui::BeginDisabled();
    if (danger) ImGui::PushStyleColor(ImGuiCol_Button, theme::background(theme::Tone::danger));
    pressed = ImGui::Button(label);
    if (danger) ImGui::PopStyleColor();
    if (!enabled) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Unavailable");
    }
}

}  // namespace

TopBarAction draw_top_bar(const dashboard_snapshot* snapshot, const CommandCenterViewModel* view,
                          bool paused, bool pause_available, bool pause_state_available,
                          bool flatten_available, bool kill_available)
{
    const char* target =
        snapshot && !snapshot->debug.target.empty() ? snapshot->debug.target.c_str() : "WAITING";
    const bool halted = snapshot && snapshot->risk.halted;
    const bool live = is_live_target(target);
    const bool narrow = ImGui::GetContentRegionAvail().x < 1'150.0f;
    TopBarAction action = TopBarAction::none;

    ImGui::BeginChild("top_status_bar", {0.0f, narrow ? 68.0f : 34.0f}, true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored(theme::text(), "TrueTest");
    ImGui::SameLine();
    widgets::status_badge(target_label(target), live ? theme::Tone::danger : theme::Tone::accent);
    if (snapshot) {
        ImGui::SameLine();
        widgets::status_badge(
            snapshot->health.provider_name.empty() ? "PROVIDER UNKNOWN"
                                                   : snapshot->health.provider_name.c_str(),
            snapshot->health.provider_present ? theme::Tone::neutral : theme::Tone::muted);
        ImGui::SameLine();
        widgets::status_badge(provider_state(snapshot->health.provider_state),
                              provider_tone(snapshot->health.provider_state));
    } else {
        ImGui::SameLine();
        widgets::status_badge("SNAPSHOT UNAVAILABLE", theme::Tone::muted);
    }
    ImGui::SameLine();
    if (view && view->snapshot_stale)
        widgets::status_badge("UPDATE STALE", theme::Tone::danger);
    else if (view && view->snapshot_age_ms)
        widgets::status_badge("UPDATE CURRENT", theme::Tone::accent);
    else
        widgets::status_badge("DATA AGE UNKNOWN", theme::Tone::warning);
    if (halted) {
        ImGui::SameLine();
        widgets::status_badge("HALTED — RESTART REQUIRED", theme::Tone::danger);
    } else if (paused) {
        ImGui::SameLine();
        widgets::status_badge("PAUSED", theme::Tone::warning);
    } else if (pause_available && !pause_state_available) {
        ImGui::SameLine();
        widgets::status_badge("PAUSE STATE UNKNOWN", theme::Tone::warning);
    }
    if (view && !view->selected_symbol.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(theme::text_muted(), "%s", view->selected_symbol.c_str());
    }

    if (!narrow)
        ImGui::SameLine(ImGui::GetWindowWidth() - 260.0f);
    else
        ImGui::NewLine();
    bool pressed = false;
    disabled_button(paused ? "Resume" : "Pause", pause_available, false, pressed);
    if (pressed) action = TopBarAction::pause_toggle;
    ImGui::SameLine();
    disabled_button("Flatten…", flatten_available, true, pressed);
    if (pressed) action = TopBarAction::flatten;
    ImGui::SameLine();
    disabled_button("Kill…", kill_available, true, pressed);
    if (pressed) action = TopBarAction::kill;
    ImGui::EndChild();
    return action;
}

}  // namespace truetest::ui::desk::panels

#endif  // HAS_IMGUI_DESK
