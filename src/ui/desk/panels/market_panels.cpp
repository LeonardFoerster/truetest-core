#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/market_panels.h"

#include "ui/desk/desk_theme.h"
#include "ui/desk/desk_window_names.h"
#include "ui/desk/format_scale.h"
#include "ui/desk/monitor_model.h"
#include "ui/desk/panels/panel_helpers.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>

namespace truetest::ui::desk::panels {
namespace {

void depth_bar(float fraction, ImVec4 color, float row_height)
{
    const float frac = std::clamp(fraction, 0.0f, 1.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1{p0.x + ImGui::GetContentRegionAvail().x * frac, p0.y + row_height};
    ImGui::GetWindowDrawList()->AddRectFilled(p0, p1, theme::u32(color));
}

void imbalance_bar(float bid_fraction)
{
    const float fraction = std::clamp(bid_fraction, 0.0f, 1.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    constexpr float height = 9.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, ImVec2(p.x + width * fraction, p.y + height),
                        theme::u32(theme::up_dim()), 2.0f);
    draw->AddRectFilled(ImVec2(p.x + width * fraction, p.y),
                        ImVec2(p.x + width, p.y + height),
                        theme::u32(theme::down_dim()), 2.0f);
    draw->AddLine(ImVec2(p.x + width * fraction, p.y - 1.0f),
                  ImVec2(p.x + width * fraction, p.y + height + 1.0f),
                  theme::u32(theme::tx_mid()), 1.0f);
    ImGui::Dummy(ImVec2(width, height));
}

void draw_analysis_placeholder(DeskPanel panel,
                               const char* purpose,
                               const char* first,
                               const char* second,
                               const char* third)
{
    if (!ImGui::Begin(desk_window_name(panel)))
    {
        ImGui::End();
        return;
    }

    theme::section_header("ANALYSIS FRAME", purpose, theme::secondary());
    theme::status_badge("NOT CONNECTED", theme::StatusTone::neutral);
    ImGui::TextWrapped(
        "This workspace is intentionally presentation-only. Analysis data and "
        "decision logic will be connected in a later change.");
    ImGui::Spacing();

    if (ImGui::BeginTable("analysis_frame_slots", 3,
                          ImGuiTableFlags_SizingStretchSame
                              | ImGuiTableFlags_NoSavedSettings))
    {
        const char* labels[] = {first, second, third};
        for (const char* label : labels)
        {
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::bg2());
            ImGui::BeginChild(label, ImVec2(0, 72), true,
                              ImGuiWindowFlags_NoScrollbar);
            ImGui::TextColored(theme::tx_mid(), "%s", label);
            ImGui::TextColored(theme::tx_faint(), "Awaiting data wiring");
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace

void draw_ticker_strip(const dashboard_snapshot& snap)
{
    const bool narrow = ImGui::GetContentRegionAvail().x < 780.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::bg1());
    ImGui::BeginChild("ticker", ImVec2(0, narrow ? 58.0f : 36.0f), true);

    if (snap.l2.source != dashboard_snapshot::l2_source::none && !snap.l2.symbol.empty())
    {
        ImGui::SetWindowFontScale(1.08f);
        ImGui::TextColored(theme::accent(), "%s", snap.l2.symbol.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SameLine();
        ImGui::TextColored(theme::tx_hi(), "  %s", fmt_px(snap.l2.mid).c_str());
        const char* source = snap.l2.source == dashboard_snapshot::l2_source::venue
            ? "VENUE" : "SYNTH";
        ImGui::SameLine();
        theme::status_badge(source,
                            snap.l2.source == dashboard_snapshot::l2_source::venue
                                ? theme::StatusTone::info : theme::StatusTone::warning);
        if (!narrow && !snap.health.provider_name.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(theme::tx_faint(), "%s", snap.health.provider_name.c_str());
        }
        if (narrow)
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
        else
            ImGui::SameLine();
        ImGui::TextColored(theme::tx_lo(), "B %s  A %s  ·  %s",
                           fmt_px(snap.l2.best_bid).c_str(),
                           fmt_px(snap.l2.best_ask).c_str(),
                           fmt_bps(snap.l2.spread_bps).c_str());
    }
    else if (!snap.positions.empty())
    {
        for (std::size_t i = 0; i < snap.positions.size(); ++i)
        {
            const auto& position = snap.positions[i];
            if (i) ImGui::SameLine(0, 18);
            ImGui::TextColored(theme::accent(), "%s", position.symbol.c_str());
            ImGui::SameLine();
            ImGui::Text("%s", fmt_px(position.mark).c_str());
            ImGui::SameLine();
            text_pnl(position.unrealized);
        }
    }
    else
    {
        ImGui::TextColored(theme::tx_faint(), "No market data yet — waiting for L2 / marks");
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

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

void draw_l2_panel(const dashboard_snapshot& snap)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::order_book)))
    {
        ImGui::End();
        return;
    }

    if (snap.l2.source == dashboard_snapshot::l2_source::none)
    {
        ImGui::TextColored(theme::tx_faint(), "No L2 depth for this session.");
        ImGui::TextColored(theme::tx_lo(), "Shadow: enable --depth-stream  ·  Paper: MM book");
        ImGui::End();
        return;
    }

    theme::section_header("DEPTH", "top-of-book liquidity", theme::accent());
    ImGui::TextColored(theme::accent(), "%s", snap.l2.symbol.c_str());
    ImGui::SameLine();
    ImGui::Text("  mid %s", fmt_px(snap.l2.mid).c_str());
    ImGui::SameLine();
    ImGui::TextColored(theme::tx_lo(), "  %s", fmt_bps(snap.l2.spread_bps).c_str());
    ImGui::SameLine();
    theme::status_badge(snap.l2.source == dashboard_snapshot::l2_source::venue
                            ? "VENUE" : "SYNTH",
                        snap.l2.source == dashboard_snapshot::l2_source::venue
                            ? theme::StatusTone::info : theme::StatusTone::warning);

    const double total = snap.l2.cum_bid_size + snap.l2.cum_ask_size;
    const float imbalance = total > 0.0
        ? static_cast<float>(snap.l2.cum_bid_size / total) : 0.5f;
    ImGui::TextColored(theme::tx_lo(), "imbalance");
    ImGui::SameLine();
    imbalance_bar(imbalance);
    ImGui::TextColored(theme::up(), "%.0f%% bid", imbalance * 100.0f);
    ImGui::SameLine();
    ImGui::TextColored(theme::down(), "  %.0f%% ask", (1.0f - imbalance) * 100.0f);

    double max_size = 1e-12;
    for (const auto& level : snap.l2.bids)
        max_size = std::max(max_size, level.size);
    for (const auto& level : snap.l2.asks)
        max_size = std::max(max_size, level.size);
    const float row_height = ImGui::GetTextLineHeightWithSpacing();

    if (ImGui::BeginTable("asks", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Price");
        ImGui::TableSetupColumn("Cum");
        ImGui::TableHeadersRow();
        for (auto it = snap.l2.asks.rbegin(); it != snap.l2.asks.rend(); ++it)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            depth_bar(static_cast<float>(it->size / max_size), theme::down_dim(), row_height);
            ImGui::Text("%s", fmt_qty(it->size).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::down(), "%s", fmt_px(it->price).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::tx_faint(), "%s", fmt_qty(it->cum).c_str());
        }
        ImGui::EndTable();
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::secondary_dim());
    ImGui::BeginChild("spread_mid", ImVec2(0, 31), true);
    ImGui::TextColored(theme::secondary(), "  SPREAD %s", 
                       fmt_bps(snap.l2.spread_bps).c_str());
    ImGui::SameLine();
    ImGui::TextColored(theme::tx_mid(), "  MICRO %s",
                       fmt_px(snap.l2.microprice).c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (ImGui::BeginTable("bids", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        for (const auto& level : snap.l2.bids)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            depth_bar(static_cast<float>(level.size / max_size), theme::up_dim(), row_height);
            ImGui::Text("%s", fmt_qty(level.size).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::up(), "%s", fmt_px(level.price).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::tx_faint(), "%s", fmt_qty(level.cum).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void draw_fills_panel(const dashboard_snapshot& snap,
                      int& filter,
                      std::vector<std::size_t>& visible_rows)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::fills)))
    {
        ImGui::End();
        return;
    }

    ImGui::TextColored(theme::tx_lo(), "%zu recent", snap.recent_fills.size());
    ImGui::SameLine(0, 16);
    ImGui::RadioButton("All", &filter, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Buys", &filter, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Sells", &filter, 2);
    build_visible_fill_rows(snap, filter, visible_rows);

    if (ImGui::BeginTable("fills", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                              | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, -1)))
    {
        ImGui::TableSetupColumn("Sym");
        ImGui::TableSetupColumn("Side");
        ImGui::TableSetupColumn("Qty");
        ImGui::TableSetupColumn("Price");
        ImGui::TableSetupColumn("Fee");
        ImGui::TableSetupColumn("Src");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(clipper_count(visible_rows.size()));
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const auto& fill = snap.recent_fills[visible_rows[static_cast<std::size_t>(i)]];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(theme::accent(), "%s", fill.symbol.c_str());
                ImGui::TableNextColumn();
                side_badge(fill.side);
                ImGui::TableNextColumn();
                mono_text(fmt_qty(fill.qty).c_str());
                ImGui::TableNextColumn();
                mono_text(fmt_px(fill.price).c_str());
                ImGui::TableNextColumn();
                mono_text(fmt_num(fill.fee, 4).c_str());
                ImGui::TableNextColumn();
                theme::status_badge(fill.source && fill.source[0] ? fill.source : "N/A",
                                    fill.source && fill.source[0]
                                        ? theme::StatusTone::info
                                        : theme::StatusTone::neutral);
            }
        }
        ImGui::EndTable();
    }
    if (snap.recent_fills.empty())
        ImGui::TextColored(theme::tx_faint(), "No fills yet");
    else if (visible_rows.empty())
        ImGui::TextColored(theme::tx_faint(), "No fills match filter");
    ImGui::End();
}

void draw_analysis_frame_panels()
{
    draw_analysis_placeholder(DeskPanel::market_chart, "price, trend and structure",
                              "Price history", "Trend overlays", "Annotations");
    draw_analysis_placeholder(DeskPanel::indicators, "multi-factor decision support",
                              "Trend", "Momentum", "Volatility");
    draw_analysis_placeholder(DeskPanel::volume_flow, "participation and liquidity",
                              "Volume profile", "Order-flow balance", "Liquidity");
    draw_analysis_placeholder(DeskPanel::market_activity, "recent market evidence",
                              "Trade tape", "Session events", "Catalysts");
    draw_analysis_placeholder(DeskPanel::signal_checklist, "pre-decision discipline",
                              "Setup", "Regime", "Risk / reward");
    draw_analysis_placeholder(DeskPanel::market_context, "venue and session context",
                              "Session", "Data quality", "Market state");
}

} // namespace truetest::ui::desk::panels

#endif // HAS_IMGUI_DESK
