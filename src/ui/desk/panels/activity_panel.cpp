#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/activity_panel.h"

#include "ui/desk/desk_theme.h"
#include "ui/desk/format_scale.h"
#include "ui/desk/panels/panel_helpers.h"

#include "imgui.h"

#include <algorithm>

namespace truetest::ui::desk::panels {
namespace {

void empty_row(const char* text)
{
    ImGui::Spacing();
    ImGui::TextColored(theme::tx_faint(), "%s", text);
}

void draw_positions(const dashboard_snapshot& snap, float row_height, const char* symbol_filter)
{
    if (snap.positions.empty()
        || (symbol_filter && std::none_of(snap.positions.begin(), snap.positions.end(),
            [symbol_filter](const auto& row) { return row.symbol == symbol_filter; })))
    {
        empty_row(symbol_filter ? "No open positions for selected symbol" : "No open positions");
        return;
    }
    static constexpr TableColumn kPositionsColumns[] = {
        {"SYMBOL"}, {"SIDE"},
        {"QTY", ImGuiTableColumnFlags_WidthStretch},
        {"ENTRY", ImGuiTableColumnFlags_WidthStretch},
        {"MARK", ImGuiTableColumnFlags_WidthStretch},
        {"UPNL", ImGuiTableColumnFlags_WidthStretch},
    };
    if (!begin_table("activity_positions", kPositionsColumns,
                     ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
                         | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                     ImVec2(0, -1), /*freeze_cols=*/0, /*freeze_rows=*/1))
        return;
    for (const auto& position : snap.positions)
    {
        if (symbol_filter && position.symbol != symbol_filter) continue;
        ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
        ImGui::TableNextColumn(); ImGui::TextColored(theme::data_link(), "%s", position.symbol.c_str());
        ImGui::TableNextColumn(); position_side_badge(position.qty);
        ImGui::TableNextColumn(); mono_text(fmt_qty(abs_qty(position.qty)).c_str());
        ImGui::TableNextColumn(); mono_text(fmt_px(position.avg_entry).c_str());
        ImGui::TableNextColumn(); mono_text(fmt_px(position.mark).c_str());
        ImGui::TableNextColumn(); text_pnl(position.unrealized);
    }
    ImGui::EndTable();
}

void draw_orders(const dashboard_snapshot& snap, float row_height, const char* symbol_filter)
{
    if (snap.open_orders.empty()
        || (symbol_filter && std::none_of(snap.open_orders.begin(), snap.open_orders.end(),
            [symbol_filter](const auto& row) { return row.symbol == symbol_filter; })))
    {
        empty_row(symbol_filter ? "No working orders for selected symbol" : "No working orders");
        return;
    }
    static constexpr TableColumn kOrdersColumns[] = {
        {"ID"}, {"SYMBOL"}, {"SIDE"}, {"TYPE"}, {"QTY"}, {"PRICE"}, {"STATE"},
    };
    if (!begin_table("activity_orders", kOrdersColumns,
                     ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
                         | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                     ImVec2(0, -1), /*freeze_cols=*/0, /*freeze_rows=*/1))
        return;
    for (const auto& order : snap.open_orders)
    {
        if (symbol_filter && order.symbol != symbol_filter) continue;
        ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
        ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(order.order_id));
        ImGui::TableNextColumn(); ImGui::TextColored(theme::data_link(), "%s", order.symbol.c_str());
        ImGui::TableNextColumn(); side_badge(order.side);
        ImGui::TableNextColumn(); ImGui::Text("%c", order.type);
        ImGui::TableNextColumn(); mono_text(fmt_qty(order.qty).c_str());
        ImGui::TableNextColumn(); mono_text(fmt_px(order.price).c_str());
        ImGui::TableNextColumn(); order_status_badge(order.status);
    }
    ImGui::EndTable();
}

void draw_fills(const dashboard_snapshot& snap, float row_height, const char* symbol_filter)
{
    if (snap.recent_fills.empty()
        || (symbol_filter && std::none_of(snap.recent_fills.begin(), snap.recent_fills.end(),
            [symbol_filter](const auto& row) { return row.symbol == symbol_filter; })))
    {
        empty_row(symbol_filter ? "No recent fills for selected symbol"
                                : "No fills in the snapshot window");
        return;
    }
    static constexpr TableColumn kFillsColumns[] = {
        {"SYMBOL"}, {"SIDE"}, {"QTY"}, {"PRICE"}, {"FEE"}, {"SOURCE"},
    };
    if (!begin_table("activity_fills", kFillsColumns,
                     ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
                         | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                     ImVec2(0, -1), /*freeze_cols=*/0, /*freeze_rows=*/1))
        return;
    if (!symbol_filter)
    {
        ImGuiListClipper clipper;
        clipper.Begin(clipper_count(snap.recent_fills.size()), row_height);
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const auto& fill = snap.recent_fills[static_cast<std::size_t>(i)];
                ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
                ImGui::TableNextColumn(); ImGui::TextColored(theme::data_link(), "%s", fill.symbol.c_str());
                ImGui::TableNextColumn(); side_badge(fill.side);
                ImGui::TableNextColumn(); mono_text(fmt_qty(fill.qty).c_str());
                ImGui::TableNextColumn(); mono_text(fmt_px(fill.price).c_str());
                ImGui::TableNextColumn(); mono_text(fmt_num(fill.fee, 4).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(fill.source && fill.source[0] ? fill.source : "N/A");
            }
        }
    }
    else
        for (const auto& fill : snap.recent_fills)
        {
            if (fill.symbol != symbol_filter) continue;
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
            ImGui::TableNextColumn(); ImGui::TextColored(theme::data_link(), "%s", fill.symbol.c_str());
            ImGui::TableNextColumn(); side_badge(fill.side);
            ImGui::TableNextColumn(); mono_text(fmt_qty(fill.qty).c_str());
            ImGui::TableNextColumn(); mono_text(fmt_px(fill.price).c_str());
            ImGui::TableNextColumn(); mono_text(fmt_num(fill.fee, 4).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(fill.source && fill.source[0] ? fill.source : "N/A");
        }
    ImGui::EndTable();
}

void draw_protection(const dashboard_snapshot& snap, float row_height, const char* symbol_filter)
{
    if ((snap.brackets.empty() && snap.lots.empty())
        || (symbol_filter && std::none_of(snap.brackets.begin(), snap.brackets.end(),
            [symbol_filter](const auto& row) { return row.symbol == symbol_filter; })))
    {
        empty_row(symbol_filter ? "No protective brackets for selected symbol"
                                : "No active lots or protective brackets");
        return;
    }
    static constexpr TableColumn kProtectionColumns[] = {
        {"SYMBOL"}, {"SIDE"}, {"QTY"}, {"ENTRY"}, {"STOP"}, {"TARGET"}, {"OWNER"},
    };
    if (!begin_table("activity_protection", kProtectionColumns,
                     ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
                         | ImGuiTableFlags_ScrollY, ImVec2(0, -1)))
        return;
    for (const auto& bracket : snap.brackets)
    {
        if (symbol_filter && bracket.symbol != symbol_filter) continue;
        ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
        ImGui::TableNextColumn(); ImGui::TextColored(theme::data_link(), "%s", bracket.symbol.c_str());
        ImGui::TableNextColumn(); position_side_badge(bracket.side);
        ImGui::TableNextColumn(); mono_text(fmt_qty(bracket.qty).c_str());
        ImGui::TableNextColumn(); mono_text(fmt_px(bracket.entry_price).c_str());
        ImGui::TableNextColumn(); mono_text(bracket.stop_loss ? fmt_px(*bracket.stop_loss).c_str() : "—");
        ImGui::TableNextColumn(); mono_text(bracket.take_profit ? fmt_px(*bracket.take_profit).c_str() : "—");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(bracket.venue_managed ? "VENUE" : "ENGINE");
    }
    ImGui::EndTable();
}

} // namespace

void draw_activity_panel(DeskPanel panel,
                         const dashboard_snapshot* snap,
                         DeskDensity density,
                         const char* symbol_filter)
{
    if (!ImGui::Begin(desk_window_name(panel)))
    {
        ImGui::End();
        return;
    }
    theme::panel_meta(symbol_filter ? "SELECTED-SYMBOL ACTIVITY" : "EXECUTION ACTIVITY",
                      symbol_filter,
                      snap ? DeskDataState::snapshot : DeskDataState::unavailable, 0);
    if (!snap)
    {
        empty_row("Waiting for a coherent engine snapshot");
        ImGui::End();
        return;
    }

    const float row_height = theme::dp(desk_row_height(density));
    if (ImGui::BeginTabBar("activity_tabs", ImGuiTabBarFlags_FittingPolicyScroll))
    {
        if (ImGui::BeginTabItem("Positions")) { draw_positions(*snap, row_height, symbol_filter); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Working")) { draw_orders(*snap, row_height, symbol_filter); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Fills")) { draw_fills(*snap, row_height, symbol_filter); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Protection")) { draw_protection(*snap, row_height, symbol_filter); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Closed")) { empty_row("Closed-trade attribution is not connected yet"); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Alerts")) { empty_row("No desk alerts"); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace truetest::ui::desk::panels

#endif
