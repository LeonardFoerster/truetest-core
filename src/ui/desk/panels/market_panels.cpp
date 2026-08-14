#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/market_panels.h"

#include "ui/desk/desk_theme.h"
#include "ui/desk/desk_window_names.h"
#include "ui/desk/format_scale.h"

#include "imgui.h"
#include "implot.h"

namespace truetest::ui::desk::panels {

void draw_equity_panel(const dashboard_snapshot& snap)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::equity)))
    {
        ImGui::End();
        return;
    }

    const double current = snap.trend.equity_now > 0
        ? snap.trend.equity_now : snap.equity;
    theme::section_header("SESSION EQUITY", "current and peak-to-current drawdown",
                          theme::secondary());
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextColored(theme::pnl_color(current - snap.initial_balance), "%s",
                       fmt_full_usd(current).c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine(0, 18);
    ImGui::TextColored(theme::tx_lo(), "DD");
    ImGui::SameLine();
    ImGui::TextColored(theme::down(), "%s",
                       fmt_pct_abs(snap.trend.drawdown_now_pct, true).c_str());
    ImGui::SameLine(0, 16);
    ImGui::TextColored(theme::tx_lo(), "SHARPE");
    ImGui::SameLine();
    ImGui::TextColored(snap.perf.sharpe < 0.0 ? theme::down() : theme::secondary(),
                       "%.2f", snap.perf.sharpe);

    const auto& equity = snap.trend.equity_tail;
    const auto& drawdown = snap.trend.drawdown_tail;
    if (equity.size() >= 2)
    {
        ImPlot::PushStyleColor(ImPlotCol_FrameBg, theme::u32(theme::bg0()));
        ImPlot::PushStyleColor(ImPlotCol_PlotBg, theme::u32(theme::bg0()));
        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, theme::u32(theme::line()));
        if (ImPlot::BeginPlot("##eq", ImVec2(-1, ImGui::GetContentRegionAvail().y * 0.62f),
                              ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect))
        {
            ImPlot::SetupAxes(nullptr, nullptr,
                              ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_AutoFit,
                              ImPlotAxisFlags_AutoFit);
            ImPlot::PushStyleColor(ImPlotCol_Line, theme::u32(theme::secondary()));
            ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 2.0f);
            ImPlot::PlotLine("equity", equity.data(), static_cast<int>(equity.size()));
            ImPlot::PopStyleVar();
            ImPlot::PopStyleColor();
            ImPlot::EndPlot();
        }
        if (drawdown.size() >= 2
            && ImPlot::BeginPlot("##dd", ImVec2(-1, -1),
                                 ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect))
        {
            ImPlot::SetupAxes(nullptr, "dd %",
                              ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_AutoFit,
                              ImPlotAxisFlags_AutoFit);
            ImPlot::PushStyleColor(ImPlotCol_Line, theme::u32(theme::down()));
            ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 1.5f);
            ImPlot::PlotLine("dd", drawdown.data(), static_cast<int>(drawdown.size()));
            ImPlot::PopStyleVar();
            ImPlot::PopStyleColor();
            ImPlot::EndPlot();
        }
        ImPlot::PopStyleColor(3);
    }
    else
    {
        ImGui::Spacing();
        ImGui::TextColored(theme::tx_faint(),
                           "Equity series building… (%zu pts, need ≥ 2)", equity.size());
        ImGui::Text("Equity %s   initial %s",
                    fmt_full_usd(snap.equity).c_str(),
                    fmt_full_usd(snap.initial_balance).c_str());
    }
    ImGui::End();
}

} // namespace truetest::ui::desk::panels

#endif // HAS_IMGUI_DESK
