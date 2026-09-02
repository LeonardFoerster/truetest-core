#ifdef HAS_RICH_TUI

#include "risk_panel.h"

#include "../console_dashboard.h"
#include "../dashboard_snapshot.h"
#include "../snapshot_metrics.h"
#include "../tui_style.h"

#include <ncurses.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace truetest::ui {

namespace {

constexpr int kBarWidth = 28;

const char* sev_text(event_severity s)
{
    switch (s)
    {
        case event_severity::info:   return "info ";
        case event_severity::notice: return "note ";
        case event_severity::warn:   return "warn ";
        case event_severity::error:  return "error";
    }
    return "?    ";
}



std::string fmt_hhmmss(std::chrono::system_clock::time_point tp)
{
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

}

void RiskPanel::draw(int body_y0, int width, int height,
                     const ConsoleDashboard& data,
                     const dashboard_snapshot* snap)
{
    if (!snap)
    {
        mvaddstr(body_y0, 2, "(no snapshot yet - engine warming up)");
        return;
    }

    int y = body_y0;
    int y_end = body_y0 + height;

    // Status line - now using semantic colors
    draw_label(y, 2, "Status");
    {
        Color status_color = snap->risk.halted ? Color::Danger : Color::Positive;
        set_color_bold(status_color);
        mvaddstr(y, 14, snap->risk.halted ? "HALTED  ⚠" : "RUNNING");
        unset_color_bold(status_color);
    }
    ++y;

    if (y < y_end) mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Overall Risk Level (new in Phase 2) ──
    const auto assessment = assess_snapshot_risk(snap->risk);

    // Overall Risk Level - very prominent (critical for futures)
    draw_label(y, 2, "Overall Risk");
    if (assessment.level == snapshot_risk_level::unknown)
    {
        set_color_bold(Color::Warning);
        mvaddstr(y, 16, "UNKNOWN");
        unset_color_bold(Color::Warning);
    }
    else
    {
        const auto overall_risk = [&] {
            switch (assessment.level)
            {
                case snapshot_risk_level::safe: return RiskLevel::Safe;
                case snapshot_risk_level::caution: return RiskLevel::Caution;
                case snapshot_risk_level::warning: return RiskLevel::Warning;
                case snapshot_risk_level::danger: return RiskLevel::Danger;
                case snapshot_risk_level::critical: return RiskLevel::Critical;
                case snapshot_risk_level::unknown: break;
            }
            return RiskLevel::Warning;
        }();
        Color risk_col = risk_level_to_color(overall_risk);
        set_color_bold(risk_col);
        mvaddstr(y, 16, risk_level_to_string(overall_risk));
        if (!assessment.complete) mvaddstr(y, 25, " (partial data)");
        unset_color_bold(risk_col);
    }
    ++y;

    if (y < y_end) mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Risk Limits ──
    draw_label(y++, 2, "Risk Limits");
    if (y >= y_end) return;

    // Helper for risk limits - gives stronger treatment to the really dangerous ones
    auto risk_limit_row = [&](const char* lbl, double cur, double limit,
                              bool available, const char* unit,
                              bool is_critical = false) {
        if (y >= y_end) return;

        draw_label(y, 2, lbl);

        if (!available || !std::isfinite(cur))
        {
            set_color_bold(Color::Warning);
            mvaddstr(y, 26, "N/A");
            unset_color_bold(Color::Warning);
        }
        else if (limit > 0.0)
        {
            double frac = cur / limit;
            Color bar_c = (frac >= 0.9) ? Color::Danger :
                          (frac >= 0.7) ? Color::Danger :
                          (frac >= 0.5) ? Color::Warning : Color::Positive;

            draw_bar(y, 26, kBarWidth, frac, bar_c);

            // Make the text more prominent for critical limits (Daily Loss & Drawdown)
            if (is_critical) set_color_bold(bar_c);
            char b[64];
            std::snprintf(b, sizeof(b), "%.2f%s / %.2f%s", cur, unit, limit, unit);
            mvaddstr(y, 64, b);
            if (is_critical) unset_color_bold(bar_c);
        }
        else
        {
            attron(A_DIM);
            mvaddstr(y, 26, "(no limit set)");
            attroff(A_DIM);
            char b[32];
            std::snprintf(b, sizeof(b), "%.2f%s", cur, unit);
            mvaddstr(y, 64, b);
        }
        ++y;
    };

    risk_limit_row("Drawdown", std::abs(snap->risk.max_drawdown_pct),
                   snap->risk.max_drawdown_limit,
                   snap->risk.max_drawdown_available, "%", true);
    risk_limit_row("Daily Loss", snap->risk.daily_loss,
                   snap->risk.daily_loss_limit,
                   snap->risk.daily_loss_available, "", true);
    risk_limit_row("Exposure", snap->risk.exposure,
                   snap->risk.exposure_limit,
                   snap->risk.exposure_available, "");
    risk_limit_row("Open Orders", static_cast<double>(snap->risk.open_orders), 
                   static_cast<double>(snap->risk.open_orders_limit), true, "");

    if (y < y_end) mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Performance ──
    draw_label(y++, 2, "Performance");
    if (y >= y_end) return;

    auto perf_row = [&](const char* lbl, double v, const char* unit) {
        if (y >= y_end) return;
        draw_label(y, 2, lbl);

        Color c = Color::Neutral;
        if (std::string(lbl).find("Sharpe") != std::string::npos) {
            c = (v > 1.0) ? Color::Positive : (v < 0.0 ? Color::Negative : Color::Neutral);
        } else if (std::string(lbl).find("Win") != std::string::npos) {
            c = (v >= 55.0) ? Color::Positive : (v >= 45.0 ? Color::Neutral : Color::Warning);
        } else if (std::string(lbl).find("markout") != std::string::npos) {
            c = (v < 0.0) ? Color::Positive : (v > 2.0 ? Color::Danger : (v > 0.5 ? Color::Warning : Color::Neutral));
        }

        set_color(c);
        char b[32];
        std::snprintf(b, sizeof(b), "%+10.4f%s", v, unit);
        mvaddstr(y, 26, b);
        unset_color(c);
        ++y;
    };

    perf_row("Sharpe (rolling)", snap->perf.sharpe, "");
    perf_row("Win rate",         snap->perf.win_rate, "%");
    perf_row("Avg markout",      snap->perf.avg_markout_bps, " bps");

    // ── Per-symbol exposure mini-grid ──
    if (y < y_end && !snap->positions.empty())
    {
        if (y < y_end) mvhline(y++, 1, ACS_HLINE, width - 2);
        if (y < y_end) label(y++, 2, "Per-symbol exposure");
        if (y < y_end)
        {
            attron(A_DIM);
            mvprintw(y, 2,  "%-10s", "Symbol");
            mvprintw(y, 14, "%-22s", "Exposure");
            const int notional_x = 50;
            safe_mvprintw(y, notional_x, width - notional_x - 1, "%14s", "Notional");
            attroff(A_DIM);
            ++y;
        }
        const auto total_exp = available_metric(
            snap->risk.exposure_available, snap->risk.exposure);
        std::size_t shown = 0;
        for (const auto& p : snap->positions)
        {
            if (y >= y_end - 1 && shown < snap->positions.size())
            {
                attron(A_DIM);
                mvprintw(y, 4, "+ %zu more …",
                         snap->positions.size() - shown);
                attroff(A_DIM);
                break;
            }
            if (y >= y_end) break;
            draw_label(y, 2, p.symbol.c_str());

            const auto notional = position_notional(p);
            if (!notional || !total_exp)
            {
                set_color_bold(Color::Warning);
                mvaddstr(y, 14, "N/A - mark/exposure unavailable");
                unset_color_bold(Color::Warning);
                mvprintw(y, 50, "%14s", "N/A");
                mvprintw(y, 66, "%8s", "N/A");
                ++y; ++shown;
                continue;
            }
            const double frac = (snap->risk.exposure_limit > 0.0)
                ? *notional / snap->risk.exposure_limit : 0.0;

            // Tiny inline gauge
            const int bw = 20;
            Color bar_c = (frac >= 0.85) ? Color::Danger : (frac >= 0.5 ? Color::Warning : Color::Positive);
            draw_bar(y, 14, bw, frac, bar_c);

            mvprintw(y, 50, "%14.2f", *notional);
            const double pct_port = (*total_exp > 0.0)
                ? *notional / *total_exp * 100.0 : 0.0;
            mvprintw(y, 66, "%7.1f%%", pct_port);
            ++y; ++shown;
        }
    }

    // Counts row
    if (y < y_end)
    {
        char b[96];
        std::snprintf(b, sizeof(b),
                      "  fills=%zu  trades=%zu  markout-n=%zu",
                      snap->perf.total_fills, snap->perf.total_trades,
                      snap->perf.markout_samples);
        attron(A_DIM);
        mvaddstr(y, 2, b);
        attroff(A_DIM);
        ++y;
    }

    if (y < y_end) mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Recent Events ──
    if (y >= y_end) return;
    draw_label(y++, 2, "Recent Events");

    int max_lines = y_end - y;
    if (max_lines <= 0) return;

    auto evs = data.recent_events_snapshot(static_cast<std::size_t>(max_lines));
    for (const auto& e : evs)
    {
        if (y >= y_end) break;
        std::string ts = fmt_hhmmss(e.ts);
        mvaddstr(y, 2, ts.c_str());
        Color sev_col = (e.sev == event_severity::error) ? Color::Danger :
                        (e.sev == event_severity::warn)  ? Color::Warning :
                        (e.sev == event_severity::notice)? Color::Accent : Color::Neutral;
        set_color(sev_col);
        mvaddstr(y, 12, sev_text(e.sev));
        unset_color(sev_col);
        int maxw = width - 19;
        if (maxw < 1) maxw = 1;
        std::string msg = e.msg;
        if (static_cast<int>(msg.size()) > maxw) msg.resize(maxw);
        mvaddstr(y, 19, msg.c_str());
        ++y;
    }
}

}

#endif // HAS_RICH_TUI
