#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/fills_table.h"

#include "ui/desk/desk_format.h"
#include "ui/desk/desk_theme.h"
#include "ui/desk/widgets/data_table_helpers.h"
#include "ui/desk/widgets/numeric_text.h"

#include "imgui.h"

#include <ctime>

namespace truetest::ui::desk::panels {
namespace {

std::string format_timestamp(std::chrono::system_clock::time_point timestamp)
{
    if (timestamp.time_since_epoch().count() == 0) return "—";
    const std::time_t value = std::chrono::system_clock::to_time_t(timestamp);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &value);
#else
    localtime_r(&value, &local);
#endif
    char output[16]{};
    std::strftime(output, sizeof(output), "%H:%M:%S", &local);
    return output;
}

}  // namespace

void draw_fills_table(const CommandCenterViewModel& view)
{
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_SizingStretchProp;
    if (!widgets::begin_table("fills_blotter",
                              {
                                  {"TIME"},
                                  {"SYMBOL"},
                                  {"SIDE"},
                                  {"QTY"},
                                  {"PRICE"},
                                  {"FEE"},
                                  {"SOURCE"},
                              },
                              flags, {0.0f, 0.0f}))
        return;

    for (const FillViewRow& row : view.fills) {
        const ImVec4 side = row.side == 'B'   ? theme::positive()
                            : row.side == 'S' ? theme::negative()
                                              : theme::text_muted();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        widgets::mono_text(format_timestamp(row.timestamp).c_str());
        ImGui::TableSetColumnIndex(1);
        widgets::mono_text(row.symbol.c_str());
        ImGui::TableSetColumnIndex(2);
        widgets::mono_text(row.side == 'B' ? "BUY" : row.side == 'S' ? "SELL" : "?", side);
        ImGui::TableSetColumnIndex(3);
        widgets::mono_text(format_quantity(std::optional<double>{row.quantity}).c_str());
        ImGui::TableSetColumnIndex(4);
        widgets::mono_text(format_price(std::optional<double>{row.price}).c_str());
        ImGui::TableSetColumnIndex(5);
        widgets::mono_text(format_money(std::optional<double>{row.fee}, true).c_str(),
                           widgets::pnl_color(-row.fee));
        ImGui::TableSetColumnIndex(6);
        ImGui::TextUnformatted(row.source.empty() ? "—" : row.source.c_str());
    }
    ImGui::EndTable();
}

}  // namespace truetest::ui::desk::panels

#endif  // HAS_IMGUI_DESK
