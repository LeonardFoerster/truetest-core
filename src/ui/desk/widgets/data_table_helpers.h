#pragma once

#ifdef HAS_IMGUI_DESK

#include "imgui.h"

#include <initializer_list>

namespace truetest::ui::desk::widgets {

struct TableColumn
{
    const char* label;
    ImGuiTableColumnFlags flags = 0;
    float width = 0.0f;
};

inline bool begin_table(const char* id, std::initializer_list<TableColumn> columns,
                        ImGuiTableFlags flags, ImVec2 size = {0.0f, 0.0f})
{
    if (!ImGui::BeginTable(id, static_cast<int>(columns.size()), flags, size)) return false;
    for (const auto& column : columns)
        ImGui::TableSetupColumn(column.label, column.flags, column.width);
    ImGui::TableHeadersRow();
    return true;
}

}  // namespace truetest::ui::desk::widgets

#endif  // HAS_IMGUI_DESK
