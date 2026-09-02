#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/account_strip.h"

#include "ui/desk/desk_format.h"
#include "ui/desk/desk_theme.h"
#include "ui/desk/widgets/numeric_text.h"

#include "imgui.h"

#include <array>

namespace truetest::ui::desk::panels {
namespace {

struct Metric
{
    const char* label;
    std::string value;
    ImVec4 color;
};

}  // namespace

void draw_account_strip(const dashboard_snapshot& snapshot, const AccountView& account)
{
    const std::array metrics = {
        Metric{"EQUITY", format_money(account.equity),
               account.equity ? theme::text() : theme::warning()},
        Metric{"TOTAL PNL", format_money(account.total_pnl, true),
               account.total_pnl ? widgets::pnl_color(*account.total_pnl) : theme::warning()},
        Metric{"REALIZED", format_money(account.realized_pnl, true),
               account.realized_pnl ? widgets::pnl_color(*account.realized_pnl) : theme::warning()},
        Metric{"UNREALIZED", format_money(account.unrealized_pnl, true),
               account.unrealized_pnl ? widgets::pnl_color(*account.unrealized_pnl)
                                      : theme::warning()},
        Metric{"EXPOSURE", format_money(account.gross_exposure),
               account.gross_exposure ? theme::text() : theme::warning()},
        Metric{"LEVERAGE",
               account.effective_leverage ? format_quantity(account.effective_leverage) + "x" : "—",
               account.effective_leverage ? theme::text() : theme::warning()},
        Metric{"CURRENT DD", format_percent(account.current_drawdown_pct),
               account.current_drawdown_pct ? theme::warning() : theme::text_faint()},
        Metric{"ORDERS", std::to_string(snapshot.risk.open_orders), theme::text()},
        Metric{"POSITIONS", std::to_string(snapshot.positions.size()), theme::text()},
    };

    ImGui::BeginChild("account_metric_strip", {0.0f, 53.0f}, true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (ImGui::BeginTable("account_metrics", static_cast<int>(metrics.size()),
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();
        for (const auto& metric : metrics) {
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::text_faint(), "%s", metric.label);
            const bool mono = theme::push_mono_font();
            ImGui::TextColored(metric.color, "%s", metric.value.c_str());
            theme::pop_mono_font(mono);
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

}  // namespace truetest::ui::desk::panels

#endif  // HAS_IMGUI_DESK
