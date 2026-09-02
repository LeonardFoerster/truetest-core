#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/protection_table.h"

#include "ui/desk/desk_format.h"
#include "ui/desk/desk_theme.h"
#include "ui/desk/widgets/data_table_helpers.h"
#include "ui/desk/widgets/numeric_text.h"

#include "imgui.h"

namespace truetest::ui::desk::panels {

void draw_protection_table(const CommandCenterViewModel& view)
{
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_SizingStretchProp;
    if (!widgets::begin_table("protection_blotter",
                              {
                                  {"SYMBOL"},
                                  {"SIDE"},
                                  {"QTY"},
                                  {"ENTRY"},
                                  {"MARK"},
                                  {"STOP LOSS"},
                                  {"TO SL"},
                                  {"TAKE PROFIT"},
                                  {"TO TP"},
                                  {"MANAGEMENT"},
                                  {"AGE"},
                              },
                              flags, {0.0f, 0.0f}))
        return;

    for (const ProtectionViewRow& row : view.protection) {
        const ImVec4 side = row.side == 'L'   ? theme::positive()
                            : row.side == 'S' ? theme::negative()
                                              : theme::text_muted();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        widgets::mono_text(row.symbol.c_str());
        ImGui::TableSetColumnIndex(1);
        widgets::mono_text(row.side == 'L' ? "LONG" : row.side == 'S' ? "SHORT" : "?", side);
        ImGui::TableSetColumnIndex(2);
        widgets::mono_text(format_quantity(std::optional<double>{row.quantity}).c_str());
        ImGui::TableSetColumnIndex(3);
        widgets::mono_text(format_price(row.entry).c_str());
        ImGui::TableSetColumnIndex(4);
        widgets::mono_text(format_price(row.mark).c_str());
        ImGui::TableSetColumnIndex(5);
        widgets::mono_text(format_price(row.stop_loss).c_str(),
                           row.stop_loss ? theme::warning() : theme::text_faint());
        ImGui::TableSetColumnIndex(6);
        widgets::mono_text(format_bps(row.distance_to_stop_bps).c_str(),
                           row.stop_loss ? theme::warning() : theme::text_faint());
        ImGui::TableSetColumnIndex(7);
        widgets::mono_text(format_price(row.take_profit).c_str(),
                           row.take_profit ? theme::positive() : theme::text_faint());
        ImGui::TableSetColumnIndex(8);
        widgets::mono_text(format_bps(row.distance_to_take_profit_bps).c_str(),
                           row.take_profit ? theme::positive() : theme::text_faint());
        ImGui::TableSetColumnIndex(9);
        ImGui::TextUnformatted(row.venue_managed ? "VENUE" : "ENGINE");
        ImGui::TableSetColumnIndex(10);
        widgets::mono_text(format_duration(row.age_seconds).c_str());
    }
    ImGui::EndTable();
}

}  // namespace truetest::ui::desk::panels

#endif  // HAS_IMGUI_DESK
