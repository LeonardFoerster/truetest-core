#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/orders_table.h"

#include "ui/desk/desk_format.h"
#include "ui/desk/desk_theme.h"
#include "ui/desk/widgets/data_table_helpers.h"
#include "ui/desk/widgets/numeric_text.h"

#include "imgui.h"

namespace truetest::ui::desk::panels {
namespace {

const char* order_type(char type)
{
    switch (type) {
    case 'M':
        return "MARKET";
    case 'L':
        return "LIMIT";
    case 'S':
        return "STOP";
    case 's':
        return "STOP-LIMIT";
    default:
        return "UNKNOWN";
    }
}

void future_button(const char* label)
{
    ImGui::BeginDisabled();
    ImGui::SmallButton(label);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Not wired yet");
}

}  // namespace

void draw_orders_table(const CommandCenterViewModel& view, DeskState& state,
                       const DeskTradeActionCapabilities& future_actions)
{
    (void)future_actions;
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_SizingStretchProp;
    if (!widgets::begin_table("orders_blotter",
                              {
                                  {"ID"},
                                  {"SYMBOL"},
                                  {"SIDE"},
                                  {"TYPE"},
                                  {"QTY"},
                                  {"PRICE / TRIGGER"},
                                  {"DIST BPS"},
                                  {"AGE"},
                                  {"STRATEGY"},
                                  {"STATUS"},
                                  {"ACTION"},
                              },
                              flags, {0.0f, 0.0f}))
        return;

    for (const OrderViewRow& row : view.orders) {
        const ImVec4 side_color = row.side == 'B'   ? theme::positive()
                                  : row.side == 'S' ? theme::negative()
                                                    : theme::text_muted();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const std::string id = std::to_string(row.order_id);
        if (ImGui::Selectable(id.c_str(), row.order_id == state.selected_order_id,
                              ImGuiSelectableFlags_SpanAllColumns)) {
            state.selected_order_id = row.order_id;
            state.selected_symbol = row.symbol;
        }
        ImGui::TableSetColumnIndex(1);
        widgets::mono_text(row.symbol.c_str());
        ImGui::TableSetColumnIndex(2);
        widgets::mono_text(row.side == 'B' ? "BUY" : row.side == 'S' ? "SELL" : "?", side_color);
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(order_type(row.type));
        ImGui::TableSetColumnIndex(4);
        widgets::mono_text(format_quantity(std::optional<double>{row.quantity}).c_str());
        ImGui::TableSetColumnIndex(5);
        if (row.trigger_price && row.price && row.type == 's') {
            const std::string price_trigger = format_price(row.price) + " / " +
                                              format_price(row.trigger_price);
            widgets::mono_text(price_trigger.c_str());
        } else {
            widgets::mono_text(
                format_price(row.trigger_price ? row.trigger_price : row.price).c_str());
        }
        ImGui::TableSetColumnIndex(6);
        widgets::mono_text(format_bps(row.distance_bps, true).c_str());
        ImGui::TableSetColumnIndex(7);
        widgets::mono_text(format_duration(row.age_seconds).c_str());
        ImGui::TableSetColumnIndex(8);
        ImGui::TextUnformatted(row.strategy.empty() ? "—" : row.strategy.c_str());
        ImGui::TableSetColumnIndex(9);
        ImGui::TextUnformatted(row.status.empty() ? "UNKNOWN" : row.status.c_str());
        ImGui::TableSetColumnIndex(10);
        ImGui::PushID(id.c_str());
        future_button("Cancel");
        ImGui::SameLine();
        future_button("Amend");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

}  // namespace truetest::ui::desk::panels

#endif  // HAS_IMGUI_DESK
