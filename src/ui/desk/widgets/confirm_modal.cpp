#ifdef HAS_IMGUI_DESK

#include "ui/desk/widgets/confirm_modal.h"

#include "ui/desk/desk_theme.h"

#include "imgui.h"

namespace truetest::ui::desk::widgets {

ConfirmResult draw_confirm_modal(ConfirmKind kind)
{
    if (kind == ConfirmKind::none) return ConfirmResult::none;
    const bool kill = kind == ConfirmKind::kill;
    const char* title = kill ? "Confirm kill" : "Confirm flatten";
    const char* message = kill ? "Kill is terminal for this process. Cancelling/halting follows "
                                 "the existing safety seam."
                               : "Flatten sends the existing attended flatten intent. Verify the "
                                 "current account and orders first.";
    ImGui::OpenPopup(title);
    ConfirmResult result = ConfirmResult::none;
    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(kill ? theme::negative() : theme::warning(), "%s",
                           kill ? "DESTRUCTIVE ACTION" : "OPERATOR CONFIRMATION");
        ImGui::Separator();
        ImGui::PushTextWrapPos(460.0f);
        ImGui::TextUnformatted(message);
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Cancel", {120.0f, 0.0f})) {
            result = ConfirmResult::cancelled;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, theme::background(theme::Tone::danger));
        if (ImGui::Button(kill ? "KILL" : "FLATTEN", {120.0f, 0.0f})) {
            result = ConfirmResult::confirmed;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }
    return result;
}

}  // namespace truetest::ui::desk::widgets

#endif  // HAS_IMGUI_DESK
