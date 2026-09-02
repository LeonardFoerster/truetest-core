#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/market_watch.h"

#include "ui/desk/desk_format.h"
#include "ui/desk/desk_theme.h"
#include "ui/desk/widgets/data_table_helpers.h"
#include "ui/desk/widgets/numeric_text.h"

#include "imgui.h"

namespace truetest::ui::desk::panels {
namespace {

const char* inventory_label(double quantity)
{
    if (quantity > 0.0) return "LONG";
    if (quantity < 0.0) return "SHORT";
    return "—";
}

bool apply_sort(DeskState& state)
{
    ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
    if (!specs || !specs->SpecsDirty || specs->SpecsCount == 0) return false;

    const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
    switch (spec.ColumnIndex) {
    case 0:
        state.market_sort = MarketWatchSort::symbol;
        break;
    case 1:
        state.market_sort = MarketWatchSort::mark;
        break;
    case 5:
        state.market_sort = MarketWatchSort::spread_bps;
        break;
    case 8:
        state.market_sort = MarketWatchSort::position;
        break;
    default:
        specs->SpecsDirty = false;
        return false;
    }
    state.market_sort_descending = spec.SortDirection == ImGuiSortDirection_Descending;
    specs->SpecsDirty = false;
    return true;
}

}  // namespace

void draw_market_watch(CommandCenterViewModel& view, DeskState& state)
{
    ImGui::TextColored(theme::text(), "MARKET WATCH");
    ImGui::SameLine();
    ImGui::TextColored(theme::text_faint(), "%zu instruments", view.market_watch.size());
    ImGui::Separator();

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Sortable;
    if (!widgets::begin_table("market_watch_table",
                              {
                                  {"SYMBOL", ImGuiTableColumnFlags_DefaultSort},
                                  {"MARK"},
                                  {"BID", ImGuiTableColumnFlags_NoSort},
                                  {"ASK", ImGuiTableColumnFlags_NoSort},
                                  {"SPR", ImGuiTableColumnFlags_NoSort},
                                  {"BPS"},
                                  {"MICRO", ImGuiTableColumnFlags_NoSort},
                                  {"IMB", ImGuiTableColumnFlags_NoSort},
                                  {"INV"},
                                  {"WORK", ImGuiTableColumnFlags_NoSort},
                              },
                              flags, {0.0f, 0.0f}))
        return;

    if (apply_sort(state))
        sort_market_watch(view.market_watch, state.market_sort,
                          state.market_sort_descending);
    for (const MarketWatchRow& row : view.market_watch) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const bool selected = row.symbol == state.selected_symbol;
        if (ImGui::Selectable(row.symbol.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
            state.selected_symbol = row.symbol;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select %s", row.symbol.c_str());

        ImGui::TableSetColumnIndex(1);
        widgets::mono_text(format_price(row.mark).c_str());
        ImGui::TableSetColumnIndex(2);
        widgets::mono_text(format_price(row.bid).c_str(), theme::positive());
        ImGui::TableSetColumnIndex(3);
        widgets::mono_text(format_price(row.ask).c_str(), theme::negative());
        ImGui::TableSetColumnIndex(4);
        widgets::mono_text(format_price(row.spread).c_str());
        ImGui::TableSetColumnIndex(5);
        widgets::mono_text(format_bps(row.spread_bps).c_str());
        ImGui::TableSetColumnIndex(6);
        widgets::mono_text(format_price(row.microprice).c_str());
        ImGui::TableSetColumnIndex(7);
        widgets::mono_text(format_percent(row.imbalance_pct).c_str());
        ImGui::TableSetColumnIndex(8);
        widgets::mono_text(inventory_label(row.position_qty),
                           row.position_qty > 0.0   ? theme::positive()
                           : row.position_qty < 0.0 ? theme::negative()
                                                    : theme::text_faint());
        ImGui::TableSetColumnIndex(9);
        const std::string work = "B" + std::to_string(row.working_buy_orders) + "/S" +
                                 std::to_string(row.working_sell_orders);
        widgets::mono_text(work.c_str());
    }
    ImGui::EndTable();
}

}  // namespace truetest::ui::desk::panels

#endif  // HAS_IMGUI_DESK
