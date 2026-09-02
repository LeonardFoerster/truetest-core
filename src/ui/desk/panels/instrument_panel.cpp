#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/instrument_panel.h"

#include "ui/desk/desk_format.h"
#include "ui/desk/desk_theme.h"
#include "ui/desk/widgets/numeric_text.h"

#include "imgui.h"

#include <algorithm>

namespace truetest::ui::desk::panels {
namespace {

const MarketWatchRow* selected_market(const CommandCenterViewModel& view, const DeskState& state)
{
    const std::string& selected =
        !state.selected_symbol.empty() ? state.selected_symbol : view.selected_symbol;
    const auto found =
        std::find_if(view.market_watch.begin(), view.market_watch.end(),
                     [&selected](const auto& row) { return row.symbol == selected; });
    return found == view.market_watch.end() ? nullptr : &*found;
}

void metric(const char* label, const std::string& value, ImVec4 color = theme::text())
{
    ImGui::TextColored(theme::text_faint(), "%s", label);
    ImGui::SameLine();
    widgets::mono_text(value.c_str(), color);
}

void future_button(const char* label, const char* detail)
{
    ImGui::BeginDisabled();
    ImGui::SmallButton(label);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", detail);
}

}  // namespace

void draw_instrument_panel(const CommandCenterViewModel& view, const dashboard_snapshot& snapshot,
                           const DeskState& state,
                           const DeskTradeActionCapabilities& future_actions)
{
    (void)future_actions;  // The typed seam exists, but no execution callback is wired in this
                           // release.
    const MarketWatchRow* market = selected_market(view, state);
    if (!market) {
        ImGui::TextColored(theme::text(), "SELECTED INSTRUMENT");
        ImGui::Separator();
        ImGui::TextColored(theme::text_faint(), "Select an instrument in Market Watch.");
        return;
    }

    ImGui::TextColored(theme::text(), "%s", market->symbol.c_str());
    ImGui::SameLine();
    ImGui::TextColored(theme::text_faint(), "SELECTED INSTRUMENT");
    ImGui::Separator();

    if (ImGui::BeginTable("instrument_bbo", 4,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        metric("BID", format_price(market->bid), theme::positive());
        ImGui::TableNextColumn();
        metric("ASK", format_price(market->ask), theme::negative());
        ImGui::TableNextColumn();
        metric("MID", format_price(market->mid));
        ImGui::TableNextColumn();
        metric("MARK", format_price(market->mark));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        metric("SPREAD", format_price(market->spread));
        ImGui::TableNextColumn();
        metric("SPREAD BPS", format_bps(market->spread_bps));
        ImGui::TableNextColumn();
        metric("MICRO", format_price(market->microprice));
        ImGui::TableNextColumn();
        metric("L2 IMB", format_percent(market->imbalance_pct, true));
        ImGui::EndTable();
    }

    ImGui::Spacing();
    const float chart_height = std::max(132.0f, ImGui::GetContentRegionAvail().y * 0.43f);
    ImGui::BeginChild("future_chart_surface", {0.0f, chart_height}, true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 region = ImGui::GetContentRegionAvail();
    const ImVec2 label = ImGui::CalcTextSize("CHART SURFACE RESERVED");
    ImGui::SetCursorPos({std::max(12.0f, (region.x - label.x) * 0.5f),
                         std::max(14.0f, (region.y - label.y) * 0.45f)});
    ImGui::TextColored(theme::text_faint(), "CHART SURFACE RESERVED");
    ImGui::SetCursorPosX(std::max(12.0f, (region.x - 210.0f) * 0.5f));
    ImGui::TextColored(theme::text_faint(), "No synthetic history or candles");
    ImGui::EndChild();

    ImGui::Spacing();
    if (ImGui::BeginTable("instrument_operations", 2,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        metric("INVENTORY", format_quantity(std::optional<double>{market->position_qty}),
               market->position_qty > 0.0   ? theme::positive()
               : market->position_qty < 0.0 ? theme::negative()
                                            : theme::text_muted());
        ImGui::TableNextColumn();
        const std::string working = "BUY " + std::to_string(market->working_buy_orders) +
                                    " / SELL " + std::to_string(market->working_sell_orders);
        metric("WORKING", working);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const std::string queue =
            snapshot.queue.available
                ? format_bps(std::optional<double>{static_cast<double>(snapshot.queue.avg_bps)})
                : "—";
        metric("QUEUE (ALL)", queue);
        ImGui::TableNextColumn();
        const std::string markout =
            snapshot.perf.markout_samples > 0
                ? format_bps(std::optional<double>{snapshot.perf.avg_markout_bps}, true)
                : "—";
        metric("MARKOUT (ALL)", markout,
               snapshot.perf.markout_samples > 0
                   ? widgets::pnl_color(-snapshot.perf.avg_markout_bps)
                   : theme::text_faint());
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextColored(theme::text_faint(), "FUTURE POSITION ACTIONS");
    future_button("Close", "Not wired yet");
    ImGui::SameLine();
    future_button("Partial close", "Not wired yet");
    ImGui::SameLine();
    future_button("Set SL", "Not wired yet");
    ImGui::SameLine();
    future_button("Set TP", "Not wired yet");
}

}  // namespace truetest::ui::desk::panels

#endif  // HAS_IMGUI_DESK
