#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/positions_table.h"

#include "ui/desk/desk_format.h"
#include "ui/desk/desk_theme.h"
#include "ui/desk/widgets/data_table_helpers.h"
#include "ui/desk/widgets/numeric_text.h"

#include "imgui.h"

namespace truetest::ui::desk::panels {

void draw_positions_table(const CommandCenterViewModel& view, DeskState& state)
{
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_SizingStretchProp;
    if (!widgets::begin_table("positions_blotter",
                              {
                                  {"SYMBOL"},
                                  {"SIDE"},
                                  {"QTY"},
                                  {"BREAK-EVEN"},
                                  {"MARK"},
                                  {"NOTIONAL"},
                                  {"UPNL"},
                                  {"UPNL %"},
                              },
                              flags, {0.0f, 0.0f}))
        return;

    for (const PositionViewRow& row : view.positions) {
        const bool long_position = row.quantity > 0.0;
        const ImVec4 side_color = long_position        ? theme::positive()
                                  : row.quantity < 0.0 ? theme::negative()
                                                       : theme::text_muted();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Selectable(row.symbol.c_str(), row.symbol == state.selected_symbol,
                              ImGuiSelectableFlags_SpanAllColumns))
            state.selected_symbol = row.symbol;
        ImGui::TableSetColumnIndex(1);
        widgets::mono_text(long_position ? "LONG" : "SHORT", side_color);
        ImGui::TableSetColumnIndex(2);
        widgets::mono_text(format_quantity(std::optional<double>{row.quantity}).c_str());
        ImGui::TableSetColumnIndex(3);
        widgets::mono_text(format_price(row.break_even_price).c_str());
        ImGui::TableSetColumnIndex(4);
        widgets::mono_text(format_price(row.mark).c_str());
        ImGui::TableSetColumnIndex(5);
        widgets::mono_text(format_money(row.notional).c_str());
        const ImVec4 pnl =
            row.unrealized_pnl ? widgets::pnl_color(*row.unrealized_pnl) : theme::text_faint();
        ImGui::TableSetColumnIndex(6);
        widgets::mono_text(format_money(row.unrealized_pnl, true).c_str(), pnl);
        ImGui::TableSetColumnIndex(7);
        widgets::mono_text(format_percent(row.unrealized_pnl_pct, true).c_str(), pnl);
    }
    ImGui::EndTable();
}

}  // namespace truetest::ui::desk::panels

#endif  // HAS_IMGUI_DESK
