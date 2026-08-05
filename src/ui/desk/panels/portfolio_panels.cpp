#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/portfolio_panels.h"

#include "ui/desk/desk_theme.h"
#include "ui/desk/desk_window_names.h"
#include "ui/desk/format_scale.h"
#include "ui/desk/panels/panel_helpers.h"

#include "imgui.h"

#include <algorithm>

namespace truetest::ui::desk::panels {
namespace {

void draw_bracket_rail(const dashboard_snapshot::bracket_row& bracket, float width)
{
    if (!bracket.stop_loss && !bracket.take_profit)
    {
        ImGui::TextColored(theme::tx_faint(), "no SL/TP");
        return;
    }

    const double entry = bracket.entry_price;
    const double mark = bracket.mark > 0.0 ? bracket.mark : entry;
    const double stop = bracket.stop_loss ? *bracket.stop_loss : entry;
    const double take = bracket.take_profit ? *bracket.take_profit : entry;
    const double low = std::min({stop, take, entry, mark});
    const double high = std::max({stop, take, entry, mark});
    const double span = high - low > 1e-12 ? high - low : 1.0;
    const auto x_of = [&](double price) {
        return static_cast<float>(((price - low) / span) * (width - 8.0)) + 4.0f;
    };

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(origin.x, origin.y + 9),
                        ImVec2(origin.x + width, origin.y + 13),
                        theme::u32(theme::bg3()), 2.0f);
    if (bracket.stop_loss)
        draw->AddCircleFilled(ImVec2(origin.x + x_of(stop), origin.y + 11),
                              4.0f, theme::u32(theme::down()));
    if (bracket.take_profit)
        draw->AddCircleFilled(ImVec2(origin.x + x_of(take), origin.y + 11),
                              4.0f, theme::u32(theme::up()));

    const float entry_x = origin.x + x_of(entry);
    draw->AddRectFilled(ImVec2(entry_x - 1, origin.y + 4),
                        ImVec2(entry_x + 1, origin.y + 18),
                        theme::u32(theme::accent()));
    const float mark_x = origin.x + x_of(mark);
    const bool favorable = (bracket.side == 'L' || bracket.side == 'l')
        ? mark >= entry : mark <= entry;
    draw->AddTriangleFilled(ImVec2(mark_x, origin.y + 2),
                            ImVec2(mark_x - 5, origin.y + 10),
                            ImVec2(mark_x + 5, origin.y + 10),
                            theme::u32(favorable ? theme::up() : theme::down()));
    ImGui::Dummy(ImVec2(width, 22.0f));
}

} // namespace

void draw_positions_panel(const dashboard_snapshot& snap)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::positions)))
    {
        ImGui::End();
        return;
    }

    double net_unrealized = 0.0;
    for (const auto& position : snap.positions)
        net_unrealized += position.unrealized;
    theme::section_header("OPEN EXPOSURE", "marked to the latest snapshot",
                          theme::accent());
    ImGui::TextColored(theme::tx_lo(), "%zu open", snap.positions.size());
    ImGui::SameLine();
    ImGui::Text("  net uPnL");
    ImGui::SameLine();
    text_pnl(net_unrealized);

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
        | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY
        | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("pos", 6, flags, ImVec2(0, -1)))
    {
        ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Qty");
        ImGui::TableSetupColumn("Entry");
        ImGui::TableSetupColumn("Mark");
        ImGui::TableSetupColumn("uPnL");
        ImGui::TableHeadersRow();
        for (const auto& position : snap.positions)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::accent(), "%s", position.symbol.c_str());
            ImGui::TableNextColumn();
            position_side_badge(position.qty);
            ImGui::TableNextColumn();
            mono_text(fmt_qty(abs_qty(position.qty)).c_str());
            ImGui::TableNextColumn();
            mono_text(fmt_px(position.avg_entry).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::tx_hi(), "%s", fmt_px(position.mark).c_str());
            ImGui::TableNextColumn();
            text_pnl(position.unrealized);
        }
        ImGui::EndTable();
    }
    if (snap.positions.empty())
        ImGui::TextColored(theme::tx_faint(), "Flat — no open positions");
    ImGui::End();
}

void draw_lots_panel(const dashboard_snapshot& snap)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::lots)))
    {
        ImGui::End();
        return;
    }

    theme::section_header("PROTECTIVE EXITS", "stop and take-profit coverage",
                          theme::warn());
    ImGui::TextColored(theme::tx_lo(), "%zu lots · %zu brackets",
                       snap.lots.size(), snap.brackets.size());
    ImGui::SameLine(0, 14);
    ImGui::TextColored(theme::down(), "SL");
    ImGui::SameLine();
    ImGui::TextColored(theme::up(), "TP");
    ImGui::SameLine();
    ImGui::TextColored(theme::accent(), "ENTRY");
    if (ImGui::BeginTabBar("lots_tabs"))
    {
        if (ImGui::BeginTabItem("Brackets"))
        {
            const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("br", 7, flags, ImVec2(0, -1)))
            {
                ImGui::TableSetupColumn("Sym", ImGuiTableColumnFlags_WidthFixed, 72);
                ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthFixed, 66);
                ImGui::TableSetupColumn("Qty");
                ImGui::TableSetupColumn("Entry");
                ImGui::TableSetupColumn("SL/TP rail", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Mgr", ImGuiTableColumnFlags_WidthFixed, 78);
                ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 48);
                ImGui::TableHeadersRow();
                for (const auto& bracket : snap.brackets)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(theme::accent(), "%s", bracket.symbol.c_str());
                    ImGui::TableNextColumn();
                    position_side_badge(bracket.side);
                    ImGui::TableNextColumn();
                    mono_text(fmt_qty(bracket.qty).c_str());
                    ImGui::TableNextColumn();
                    mono_text(fmt_px(bracket.entry_price).c_str());
                    ImGui::TableNextColumn();
                    draw_bracket_rail(bracket,
                                      std::max(120.0f, ImGui::GetContentRegionAvail().x));
                    ImGui::TableNextColumn();
                    theme::status_badge(bracket.venue_managed ? "VENUE" : "ENGINE",
                                        bracket.venue_managed ? theme::StatusTone::info
                                                              : theme::StatusTone::neutral);
                    ImGui::TableNextColumn();
                    mono_text(fmt_age(bracket.age_seconds).c_str());
                }
                ImGui::EndTable();
            }
            if (snap.brackets.empty())
                ImGui::TextColored(theme::tx_faint(), "No armed brackets");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Lots"))
        {
            if (ImGui::BeginTable("lots", 6,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                      | ImGuiTableFlags_ScrollY,
                                  ImVec2(0, -1)))
            {
                ImGui::TableSetupColumn("Id");
                ImGui::TableSetupColumn("Sym");
                ImGui::TableSetupColumn("Strat");
                ImGui::TableSetupColumn("Side");
                ImGui::TableSetupColumn("Qty / Entry");
                ImGui::TableSetupColumn("Age");
                ImGui::TableHeadersRow();
                for (const auto& lot : snap.lots)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", static_cast<unsigned long long>(lot.opener_order_id));
                    ImGui::TableNextColumn();
                    ImGui::TextColored(theme::accent(), "%s", lot.symbol.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(lot.strategy_name.c_str());
                    ImGui::TableNextColumn();
                    position_side_badge(lot.side);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s @ %s", fmt_qty(lot.qty_open).c_str(),
                                fmt_px(lot.entry_price).c_str());
                    ImGui::TableNextColumn();
                    mono_text(fmt_age(lot.age_seconds).c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void draw_orders_panel(const dashboard_snapshot& snap)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::open_orders)))
    {
        ImGui::End();
        return;
    }

    theme::section_header("WORKING ORDERS", "operator review only", theme::info());
    ImGui::TextColored(theme::tx_lo(), "%zu working", snap.open_orders.size());
    if (ImGui::BeginTable("oo", 7,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                              | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, -1)))
    {
        ImGui::TableSetupColumn("Id");
        ImGui::TableSetupColumn("Sym");
        ImGui::TableSetupColumn("Side");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Qty");
        ImGui::TableSetupColumn("Price");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(clipper_count(snap.open_orders.size()));
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const auto& order = snap.open_orders[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(order.order_id));
                ImGui::TableNextColumn();
                ImGui::TextColored(theme::accent(), "%s", order.symbol.c_str());
                ImGui::TableNextColumn();
                side_badge(order.side);
                ImGui::TableNextColumn();
                ImGui::Text("%c", order.type);
                ImGui::TableNextColumn();
                mono_text(fmt_qty(order.qty).c_str());
                ImGui::TableNextColumn();
                mono_text(fmt_px(order.price).c_str());
                ImGui::TableNextColumn();
                order_status_badge(order.status);
            }
        }
        ImGui::EndTable();
    }
    if (snap.open_orders.empty())
        ImGui::TextColored(theme::tx_faint(), "No open orders");
    ImGui::End();
}

void draw_trade_history_frame()
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::trade_history)))
    {
        ImGui::End();
        return;
    }
    theme::section_header("CLOSED TRADES", "round-trip history frame", theme::secondary());
    theme::status_badge("NOT CONNECTED", theme::StatusTone::neutral);
    ImGui::TextWrapped(
        "Closed-trade attribution and realized PnL rows will be connected later. "
        "The Fills panel remains the source for current execution history.");
    ImGui::Spacing();
    ImGui::TextColored(theme::tx_faint(),
                       "Planned columns: close time · symbol · side · size · entry · exit · realized PnL");
    ImGui::End();
}

} // namespace truetest::ui::desk::panels

#endif // HAS_IMGUI_DESK
