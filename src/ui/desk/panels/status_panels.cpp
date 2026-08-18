#ifdef HAS_IMGUI_DESK

#include "ui/desk/panels/status_panels.h"

#include "ui/desk/desk_theme.h"
#include "ui/desk/desk_layout_model.h"
#include "ui/desk/desk_window_names.h"
#include "ui/desk/format_scale.h"
#include "ui/desk/monitor_model.h"
#include "ui/desk/panels/panel_helpers.h"
#include "ui/operator_actions.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace truetest::ui::desk::panels {

void draw_account_strip(const dashboard_snapshot& snap,
                        const MonitorTelemetry&)
{
    // Trimmed to the 7 account/risk metrics that don't already live in
    // global chrome (target/mode/symbol/provider) or the Health panel
    // (rate, provider, stream) — §6/§9.1: compact bands, not a KPI card grid.
    constexpr std::size_t metric_count = 7;
    constexpr float gap = theme::kMetricCardGap;
    constexpr float card_height = theme::kMetricCardHeight;
    const float available = ImGui::GetContentRegionAvail().x;
    const std::size_t columns = kpi_column_count(available, metric_count);
    const std::size_t rows = (metric_count + columns - 1) / columns;
    const float card_width = (available - gap * static_cast<float>(columns - 1))
        / static_cast<float>(columns);
    const float strip_height = static_cast<float>(rows) * card_height
        + static_cast<float>(rows - 1) * gap;

    char equity_value[64];
    char equity_context[96];
    char pnl_value[64];
    char pnl_context[96];
    char unrealized_value[64];
    char unrealized_context[96];
    char drawdown_value[64];
    char drawdown_context[96];
    char exposure_value[64];
    char exposure_context[96];
    char orders_value[64];

    const double equity_delta = snap.equity - snap.initial_balance;
    std::snprintf(equity_value, sizeof(equity_value), "$%.2f", snap.equity);
    std::snprintf(equity_context, sizeof(equity_context), "cash $%.2f", snap.cash);
    std::snprintf(pnl_value, sizeof(pnl_value), "%+.2f", equity_delta);
    std::snprintf(pnl_context, sizeof(pnl_context), "%+.2f%% vs initial",
                  snap.trend.equity_change_pct);
    std::snprintf(unrealized_value, sizeof(unrealized_value), "%+.2f",
                  snap.unrealized_pnl);
    std::snprintf(unrealized_context, sizeof(unrealized_context), "%zu open positions",
                  snap.positions.size());
    std::snprintf(drawdown_value, sizeof(drawdown_value), "%.2f%%",
                  snap.trend.drawdown_now_pct);
    if (snap.risk.max_drawdown_limit > 0.0)
        std::snprintf(drawdown_context, sizeof(drawdown_context), "limit %.2f%%",
                      snap.risk.max_drawdown_limit);
    else
        std::snprintf(drawdown_context, sizeof(drawdown_context), "peak to current");
    std::snprintf(exposure_value, sizeof(exposure_value), "%s", fmt_usd(snap.risk.exposure).c_str());
    if (snap.risk.exposure_limit > 0.0)
        std::snprintf(exposure_context, sizeof(exposure_context), "limit %s",
                      fmt_usd(snap.risk.exposure_limit).c_str());
    else
        std::snprintf(exposure_context, sizeof(exposure_context), "no limit configured");
    if (snap.risk.open_orders_limit > 0)
        std::snprintf(orders_value, sizeof(orders_value), "%zu / %zu",
                      snap.risk.open_orders, snap.risk.open_orders_limit);
    else
        std::snprintf(orders_value, sizeof(orders_value), "%zu", snap.risk.open_orders);

    const bool exposure_warning = snap.risk.exposure_limit > 0.0
        && snap.risk.exposure / snap.risk.exposure_limit > theme::kWarnFraction;
    const bool orders_warning = snap.risk.open_orders_limit > 0
        && static_cast<double>(snap.risk.open_orders)
               / static_cast<double>(snap.risk.open_orders_limit) > theme::kWarnFraction;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("kpi_strip", ImVec2(0, strip_height), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    std::size_t index = 0;
    const auto card = [&](const char* id, const char* label, const char* value,
                          const char* context, ImVec4 color) {
        theme::metric_card(id, label, value, context, color,
                           ImVec2(card_width, card_height));
        ++index;
        if (index < metric_count && index % columns != 0)
            ImGui::SameLine(0.0f, gap);
    };
    card("kpi_equity", "EQUITY", equity_value, equity_context,
         theme::pnl_color(equity_delta));
    card("kpi_session", "SESSION PNL", pnl_value, pnl_context,
         theme::pnl_color(equity_delta));
    card("kpi_realized", "REALIZED PNL", "N/A", "accounting source pending",
         theme::tx_mid());
    card("kpi_unrealized", "UNREALIZED PNL", unrealized_value, unrealized_context,
         theme::pnl_color(snap.unrealized_pnl));
    card("kpi_drawdown", "DRAWDOWN", drawdown_value, drawdown_context,
         snap.trend.drawdown_now_pct > 0.0 ? theme::down() : theme::tx_mid());
    card("kpi_exposure", "EXPOSURE", exposure_value, exposure_context,
         exposure_warning ? theme::warn() : theme::tx_mid());
    card("kpi_orders", "OPEN ORDERS", orders_value,
         snap.risk.open_orders_limit > 0 ? "vs limit" : "no limit configured",
         orders_warning ? theme::warn() : theme::tx_mid());
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void draw_market_metric_band(const dashboard_snapshot& snap,
                             const MonitorTelemetry& telemetry,
                             const DeskLinkContext& context)
{
    // §7.6: only immediately-useful MARKET values — never account/build
    // metrics, which belong to Operations/Diagnostics chrome instead.
    const bool has_l2 = snap.l2.source != dashboard_snapshot::l2_source::none;
    const char* l2_source = snap.l2.source == dashboard_snapshot::l2_source::venue
        ? "VENUE" : (snap.l2.source == dashboard_snapshot::l2_source::synthetic
                          ? "SYNTH" : "NO L2");
    const bool queue_observed = snap.queue.avg_bps != 0
        || snap.queue.submitted_with_queue != 0 || snap.queue.filled_after_drain != 0;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::bg1());
    ImGui::BeginChild("market_metric_band", ImVec2(0, theme::kStripHeight), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::TextColored(theme::tx_faint(), "%s", context.symbol.c_str());
    ImGui::SameLine(0, 16);
    if (has_l2)
    {
        const bool mono = theme::push_mono_font();
        ImGui::TextColored(theme::tx_hi(), "MID %s", fmt_px(snap.l2.mid).c_str());
        theme::pop_mono_font(mono);
        ImGui::SameLine(0, 14);
        ImGui::TextColored(theme::tx_lo(), "SPREAD");
        ImGui::SameLine();
        ImGui::TextColored(theme::tx_mid(), "%s", fmt_bps(snap.l2.spread_bps).c_str());
        ImGui::SameLine(0, 14);
        ImGui::TextColored(theme::tx_lo(), "MICRO");
        ImGui::SameLine();
        ImGui::TextColored(theme::tx_mid(), "%s", fmt_px(snap.l2.microprice).c_str());
        ImGui::SameLine(0, 14);
        ImGui::TextColored(theme::tx_lo(), "IMB");
        ImGui::SameLine();
        ImGui::TextColored(snap.l2.imbalance > 0.0 ? theme::up()
                               : (snap.l2.imbalance < 0.0 ? theme::down() : theme::tx_mid()),
                           "%+.2f", snap.l2.imbalance);
        ImGui::SameLine(0, 14);
        theme::status_badge(l2_source, snap.l2.source == dashboard_snapshot::l2_source::venue
                                ? theme::StatusTone::info : theme::StatusTone::neutral);
    }
    else
    {
        ImGui::TextColored(theme::tx_faint(), "MID/SPREAD/MICRO/IMB — N/A (no L2 for this symbol)");
    }

    ImGui::SameLine(0, 18);
    if (telemetry.available() && telemetry.rate_available())
        ImGui::TextColored(theme::tx_mid(), "%.1f ev/s", snap.health.rate_ev_per_sec);
    else
        ImGui::TextColored(theme::tx_faint(), "rate N/A");

    if (queue_observed)
    {
        ImGui::SameLine(0, 14);
        ImGui::TextColored(theme::tx_lo(), "QUEUE");
        ImGui::SameLine();
        ImGui::TextColored(theme::tx_mid(), "%.1f%%", static_cast<double>(snap.queue.avg_bps) / 100.0);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void draw_safety_strip(const dashboard_snapshot& snap,
                       const operator_actions& actions,
                       const MonitorTelemetry& telemetry)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::bg1());
    ImGui::BeginChild("safety_strip", ImVec2(0, theme::kStripHeight), true);

    const bool paused = actions.pause_state ? actions.pause_state() : false;
    theme::status_badge(snap.risk.halted ? "HALT SET — RESTART" : "HALT not set",
                        snap.risk.halted ? theme::StatusTone::halted
                                         : theme::StatusTone::neutral);
    ImGui::SameLine();
    const char* strategy_state = snap.risk.halted
        ? "STRATEGIES BLOCKED BY HALT"
        : (paused ? "STRATEGIES PAUSED" : "STRATEGIES ENABLED");
    theme::status_badge(strategy_state,
                        snap.risk.halted ? theme::StatusTone::halted
                            : (paused ? theme::StatusTone::warning
                                      : theme::StatusTone::neutral));
    ImGui::SameLine();
    theme::status_badge(actions.kill ? "KILL hook available" : "KILL unavailable",
                        actions.kill ? theme::StatusTone::warning
                                     : theme::StatusTone::neutral);
    ImGui::SameLine();

    const char* provider_state = snap.health.provider_present
        ? provider_lifecycle_text(snap.health.provider_state) : "absent";
    ImGui::TextColored(theme::tx_lo(), "provider %s (%s)",
                       snap.health.provider_name.empty()
                           ? "N/A" : snap.health.provider_name.c_str(),
                       provider_state);
    ImGui::SameLine(0, 18);
    if (telemetry.available())
    {
        const auto state = telemetry.stream_state();
        const ImVec4 color = state == connection_state::halted
            ? theme::danger() : (state == connection_state::live ? theme::up()
                                                                  : theme::tx_lo());
        ImGui::TextColored(color, "stream %s", stream_state_text(state));
    }
    else
    {
        ImGui::TextColored(theme::tx_faint(), "stream N/A");
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void draw_strategies_panel(const dashboard_snapshot& snap)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::strategies)))
    {
        ImGui::End();
        return;
    }
    theme::section_header("STRATEGY ATTRIBUTION", "reported session metrics",
                          theme::secondary());
    static constexpr TableColumn kStrategiesColumns[] = {
        {"Name"}, {"PnL"}, {"Trades"}, {"Win%"}, {"PF"}, {"Lots"}, {"Brk"},
    };
    if (begin_table("st", kStrategiesColumns,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                        | ImGuiTableFlags_ScrollY,
                    ImVec2(0, -1)))
    {
        for (const auto& strategy : snap.strategies)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(strategy.name.c_str());
            ImGui::TableNextColumn();
            text_pnl(strategy.pnl);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", strategy.trade_count);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", strategy.win_rate);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", strategy.profit_factor);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", strategy.open_lots);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", strategy.armed_brackets);
        }
        ImGui::EndTable();
    }
    if (snap.strategies.empty())
        ImGui::TextColored(theme::tx_faint(), "No strategy attribution rows");
    ImGui::End();
}

void draw_risk_panel(const dashboard_snapshot& snap)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::risk)))
    {
        ImGui::End();
        return;
    }
    const bool drawdown_warning = snap.risk.max_drawdown_limit > 0.0
        && snap.risk.max_drawdown_pct / snap.risk.max_drawdown_limit > theme::kWarnFraction;
    const bool exposure_warning = snap.risk.exposure_limit > 0.0
        && snap.risk.exposure / snap.risk.exposure_limit > theme::kWarnFraction;
    const bool orders_warning = snap.risk.open_orders_limit > 0
        && static_cast<double>(snap.risk.open_orders)
               / static_cast<double>(snap.risk.open_orders_limit)
               > theme::kWarnFraction;
    theme::section_header("RISK ENVELOPE", "reported limits and terminal state",
                          snap.risk.halted ? theme::danger()
                                           : ((drawdown_warning || exposure_warning
                                               || orders_warning) ? theme::warn()
                                                                 : theme::accent()));
    theme::status_badge(snap.risk.halted ? "HALTED — RESTART REQUIRED"
                                         : ((drawdown_warning || exposure_warning
                                             || orders_warning) ? "LIMIT WARNING"
                                                               : "HALT NOT SET"),
                        snap.risk.halted ? theme::StatusTone::halted
                            : ((drawdown_warning || exposure_warning || orders_warning)
                                   ? theme::StatusTone::warning
                                   : theme::StatusTone::neutral));
    if (snap.risk.halted)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextWrapped(
            "HALTED — trading suspended. Halt is write-once; restart the process after review.");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    const auto gauge = [](const char* label, double used, double limit, bool percent = false) {
        const float fraction = limit > 0.0
            ? static_cast<float>(std::clamp(used / limit, 0.0, 1.0)) : 0.0f;
        ImGui::Text("%s", label);
        ImGui::SameLine(theme::kLabelColumnWidth);
        if (percent)
            ImGui::Text("%s / %s", fmt_pct_abs(used, true).c_str(),
                        fmt_pct_abs(limit, true).c_str());
        else
            ImGui::Text("%s / %s", fmt_usd(used).c_str(), fmt_usd(limit).c_str());
        const ImVec4 color = fraction > theme::kDangerFraction ? theme::danger()
            : (fraction > theme::kWarnFraction ? theme::warn() : theme::accent());
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
        ImGui::ProgressBar(fraction, ImVec2(-1, 14));
        ImGui::PopStyleColor();
    };

    ImGui::Text("Daily loss");
    ImGui::SameLine(theme::kLabelColumnWidth);
    ImGui::TextColored(theme::tx_faint(), "N/A (limit %s)",
                       fmt_usd(snap.risk.daily_loss_limit).c_str());
    gauge("Drawdown", snap.risk.max_drawdown_pct, snap.risk.max_drawdown_limit, true);
    gauge("Exposure", snap.risk.exposure, snap.risk.exposure_limit);
    ImGui::Text("Open orders");
    ImGui::SameLine(theme::kLabelColumnWidth);
    ImGui::Text("%zu / %zu", snap.risk.open_orders, snap.risk.open_orders_limit);

    ImGui::Separator();
    ImGui::TextColored(theme::tx_faint(),
                       "Performance belongs to Account / Equity; this panel owns limits only.");
    ImGui::End();
}

void draw_health_panel(const dashboard_snapshot& snap,
                       const MonitorTelemetry& telemetry)
{
    if (!ImGui::Begin(desk_window_name(DeskPanel::health)))
    {
        ImGui::End();
        return;
    }

    section_label("PROVIDER");
    const char* provider_state = snap.health.provider_present
        ? provider_lifecycle_text(snap.health.provider_state) : "absent";
    const auto provider_tone = !snap.health.provider_present
        ? theme::StatusTone::neutral
        : (snap.health.provider_state == 2 ? theme::StatusTone::positive
            : (snap.health.provider_state == 3 ? theme::StatusTone::negative
                                               : theme::StatusTone::warning));
    ImGui::TextColored(theme::tx_hi(), "%s",
                       snap.health.provider_name.empty() ? "—"
                                                         : snap.health.provider_name.c_str());
    ImGui::SameLine();
    theme::status_badge(provider_state, provider_tone);
    if (telemetry.available())
    {
        const auto state = telemetry.stream_state();
        const auto tone = state == connection_state::live ? theme::StatusTone::positive
            : (state == connection_state::halted ? theme::StatusTone::halted
                : (state == connection_state::reconnecting
                       ? theme::StatusTone::warning : theme::StatusTone::neutral));
        ImGui::SameLine();
        theme::status_badge(stream_state_text(state), tone);
    }
    else
        ImGui::TextColored(theme::tx_faint(), "stream state N/A");

    section_label("LATENCY");
    if (snap.health.tick_to_trade_samples > 0)
        ImGui::Text("tick→trade  avg %.1f μs   min %.1f   max %.1f   n %zu",
                    snap.health.avg_tick_to_trade_us,
                    snap.health.min_tick_to_trade_us,
                    snap.health.max_tick_to_trade_us,
                    snap.health.tick_to_trade_samples);
    else
        ImGui::TextColored(theme::tx_faint(), "tick→trade N/A — no samples");

    section_label("FLOW");
    if (telemetry.available())
    {
        if (telemetry.rate_available())
            ImGui::Text("events %zu   fills %zu   trades %zu   %.1f /s",
                        snap.health.events_total, snap.health.fills_total,
                        snap.health.trades_total, snap.health.rate_ev_per_sec);
        else
            ImGui::Text("events %zu   fills %zu   trades %zu   rate N/A",
                        snap.health.events_total, snap.health.fills_total,
                        snap.health.trades_total);
    }
    else
        ImGui::Text("events N/A   fills %zu   trades %zu   rate N/A",
                    snap.health.fills_total, snap.health.trades_total);
    ImGui::Text("orders estimate %zu", snap.health.orders_total);
    ImGui::TextColored(theme::tx_faint(),
                       "feed / fill / bar age N/A — freshness clocks are not exposed");

    section_label("RINGS");
    if (telemetry.available())
    {
        const auto total_drops = snap.health.ring_drops_logging
            + snap.health.ring_drops_risk + snap.health.ring_drops_stats
            + snap.health.ring_drops_observer + snap.health.ring_drops_risk_stats
            + snap.health.ring_drops_mm;
        theme::status_badge(total_drops == 0 ? "NO DROPS REPORTED" : "DROPS OBSERVED",
                            total_drops == 0 ? theme::StatusTone::neutral
                                             : theme::StatusTone::warning);
        ImGui::Text("log %llu  risk %llu  stats %llu  obs %llu  risk-stats %llu  mm %llu",
                    static_cast<unsigned long long>(snap.health.ring_drops_logging),
                    static_cast<unsigned long long>(snap.health.ring_drops_risk),
                    static_cast<unsigned long long>(snap.health.ring_drops_stats),
                    static_cast<unsigned long long>(snap.health.ring_drops_observer),
                    static_cast<unsigned long long>(snap.health.ring_drops_risk_stats),
                    static_cast<unsigned long long>(snap.health.ring_drops_mm));
    }
    else
        ImGui::TextColored(theme::tx_faint(), "drop counters N/A");

    if (snap.health.questdb.active)
    {
        section_label("QUESTDB");
        theme::status_badge(snap.health.questdb.connected ? "CONNECTED" : "DOWN",
                            snap.health.questdb.connected ? theme::StatusTone::positive
                                                          : theme::StatusTone::negative);
        ImGui::SameLine();
        ImGui::Text("pending %zu  dropped %zu  fallback %zu",
                    snap.health.questdb.pending_lines,
                    snap.health.questdb.dropped_lines,
                    snap.health.questdb.fallback_lines);
    }
    if (snap.memory.available)
    {
        section_label("MEMORY");
        ImGui::Text("RSS %.1f MiB   data-segment estimate %.1f MiB   peak RSS %.1f MiB",
                    static_cast<double>(snap.memory.rss_bytes) / (1024.0 * 1024.0),
                    static_cast<double>(snap.memory.heap_bytes) / (1024.0 * 1024.0),
                    static_cast<double>(snap.memory.peak_rss_bytes) / (1024.0 * 1024.0));
    }
    section_label("ENGINE");
    ImGui::Text("preset %s   wired worker rings %zu   pin %s   spin %s",
                snap.debug.preset.c_str(), snap.debug.worker_count,
                snap.debug.cpu_pin ? "on" : "off", snap.debug.spin_policy.c_str());
    ImGui::End();
}

} // namespace truetest::ui::desk::panels

#endif // HAS_IMGUI_DESK
