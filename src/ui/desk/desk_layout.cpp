#ifdef HAS_IMGUI_DESK

#include "ui/desk/desk_layout.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace truetest::ui::desk {
namespace {

void set_node_locked(ImGuiDockNode* node, bool locked)
{
    if (!node)
        return;
    // NoDockingOverMe (imgui_internal.h): also reject a window being dropped
    // onto this node as a new tab — NoDockingSplit alone only blocks new
    // *splits*, not tab-merges, so a locked panel could still be buried
    // under another tab without it.
    constexpr ImGuiDockNodeFlags lock_flags =
        ImGuiDockNodeFlags_NoResize | ImGuiDockNodeFlags_NoUndocking
        | ImGuiDockNodeFlags_NoDockingSplit | ImGuiDockNodeFlags_NoDockingOverMe;
    if (locked)
    {
        node->LocalFlags |= lock_flags;
        node->MergedFlags |= lock_flags;
    }
    else
    {
        node->LocalFlags &= ~lock_flags;
        node->MergedFlags &= ~lock_flags;
    }
    set_node_locked(node->ChildNodes[0], locked);
    set_node_locked(node->ChildNodes[1], locked);
}

} // namespace

void apply_desk_page_layout(unsigned int dockspace_id,
                            float width,
                            float height,
                            DeskPage page,
                            bool focus_primary)
{
    constexpr ImGuiDockNodeFlags flags = ImGuiDockNodeFlags_PassthruCentralNode;
    const ImVec2 size{std::max(width, 4.0f), std::max(height, 4.0f)};

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, flags | ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, size);

    ImGuiID primary = dockspace_id;
    ImGuiID left = 0;
    ImGuiID right = 0;
    ImGuiID right_top = 0;
    ImGuiID right_bottom = 0;
    ImGuiID bottom = 0;

    const auto geometry = desk_layout_geometry(page);
    if (focus_primary)
    {
        const auto assignments = desk_page_assignments(page);
        if (!assignments.empty())
            ImGui::DockBuilderDockWindow(desk_window_name(assignments.front().panel), primary);
        ImGui::DockBuilderFinish(dockspace_id);
        ImGui::MarkIniSettingsDirty();
        return;
    }
    if (geometry.right_ratio > 0.0f)
        ImGui::DockBuilderSplitNode(primary, ImGuiDir_Right, geometry.right_ratio,
                                    &right, &primary);
    if (geometry.bottom_ratio > 0.0f)
        ImGui::DockBuilderSplitNode(primary, ImGuiDir_Down, geometry.bottom_ratio,
                                    &bottom, &primary);
    if (geometry.left_ratio > 0.0f)
        ImGui::DockBuilderSplitNode(primary, ImGuiDir_Left, geometry.left_ratio,
                                    &left, &primary);
    if (right != 0 && geometry.right_bottom_ratio > 0.0f)
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Down,
                                    geometry.right_bottom_ratio,
                                    &right_bottom, &right_top);
    else
        right_top = right;

    const std::array<ImGuiID, static_cast<std::size_t>(DeskDockSlot::count)> slots = {
        primary,
        left,
        right_top,
        right_bottom,
        bottom,
    };

    for (const auto& assignment : desk_page_assignments(page))
    {
        const auto slot = slots[static_cast<std::size_t>(assignment.slot)];
        if (slot != 0)
            ImGui::DockBuilderDockWindow(desk_window_name(assignment.panel), slot);
    }

    ImGui::DockBuilderFinish(dockspace_id);
    ImGui::MarkIniSettingsDirty();
    if (const char* ini = ImGui::GetIO().IniFilename; ini && *ini)
        ImGui::SaveIniSettingsToDisk(ini);
}

void set_desk_layout_locked(unsigned int dockspace_id, bool locked)
{
    set_node_locked(ImGui::DockBuilderGetNode(dockspace_id), locked);
}

} // namespace truetest::ui::desk

#endif // HAS_IMGUI_DESK
