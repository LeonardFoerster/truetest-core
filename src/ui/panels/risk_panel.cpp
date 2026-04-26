#ifdef HAS_RICH_TUI

#include "risk_panel.h"

#include "../console_dashboard.h"
#include "../dashboard_snapshot.h"

#include <ncurses.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

namespace truetest::ui {

namespace {

constexpr int kPairGreen  = 1;
constexpr int kPairRed    = 2;
constexpr int kPairYellow = 3;
constexpr int kPairCyan   = 4;
constexpr int kPairWhite  = 5;
constexpr int kBarWidth   = 28;

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

int sev_pair(event_severity s)
{
    switch (s)
    {
        case event_severity::error:  return kPairRed;
        case event_severity::warn:   return kPairYellow;
        case event_severity::notice: return kPairCyan;
        default:                     return kPairWhite;
    }
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

void label(int y, int x, const char* text)
{
    attron(COLOR_PAIR(kPairCyan));
    mvaddstr(y, x, text);
    attroff(COLOR_PAIR(kPairCyan));
}

// Draw a horizontal usage gauge: [████████....] 65%
// `frac` is current/limit clamped to [0, 1]. Color escalates with usage.
void draw_gauge(int y, int x, double frac)
{
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    int filled = static_cast<int>(std::lround(frac * kBarWidth));
    int empty  = kBarWidth - filled;

    int pair = (frac >= 0.85) ? kPairRed
              : (frac >= 0.6) ? kPairYellow
              : kPairGreen;

    mvaddch(y, x, '[');
    attron(COLOR_PAIR(pair));
    for (int i = 0; i < filled; ++i)
        mvaddch(y, x + 1 + i, ACS_CKBOARD);
    attroff(COLOR_PAIR(pair));
    attron(A_DIM);
    for (int i = 0; i < empty; ++i)
        mvaddch(y, x + 1 + filled + i, ACS_BULLET);
    attroff(A_DIM);
    mvaddch(y, x + 1 + kBarWidth, ']');

    char b[16];
    std::snprintf(b, sizeof(b), " %4.0f%%", frac * 100.0);
    mvaddstr(y, x + kBarWidth + 3, b);
}

} // namespace

void RiskPanel::draw(int body_y0, int width, int height,
                     const ConsoleDashboard& data,
                     const dashboard_snapshot* snap)
{
    if (!snap)
    {
        mvaddstr(body_y0, 2, "(no snapshot yet — engine warming up)");
        return;
    }

    int y = body_y0;
    int y_end = body_y0 + height;

    // Halt banner
    label(y, 2, "Status");
    {
        int p = snap->risk.halted ? kPairRed : kPairGreen;
        attron(COLOR_PAIR(p) | A_BOLD);
        mvaddstr(y, 14, snap->risk.halted ? "HALTED  ⚠"  : "RUNNING");
        attroff(COLOR_PAIR(p) | A_BOLD);
    }
    ++y;

    if (y < y_end) mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Risk gauges ──
    label(y++, 2, "Limits");
    if (y >= y_end) return;

    auto gauge_row = [&](const char* lbl, double cur, double limit,
                         const char* unit) {
        if (y >= y_end) return;
        mvprintw(y, 2, "%-22s", lbl);
        if (limit > 0.0)
        {
            draw_gauge(y, 26, cur / limit);
            char b[64];
            std::snprintf(b, sizeof(b), "%.2f%s / %.2f%s",
                          cur, unit, limit, unit);
            mvaddstr(y, 64, b);
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

    gauge_row("Drawdown",       std::abs(snap->risk.max_drawdown_pct),
              snap->risk.max_drawdown_limit, "%");
    gauge_row("Open orders",    static_cast<double>(snap->risk.open_orders),
              static_cast<double>(snap->risk.open_orders_limit), "");
    gauge_row("Exposure",       snap->risk.exposure,
              snap->risk.exposure_limit, "");
    if (snap->risk.daily_loss_limit > 0.0)
        gauge_row("Daily loss",  snap->risk.daily_loss,
                  snap->risk.daily_loss_limit, "");

    if (y < y_end) mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Performance ──
    label(y++, 2, "Performance");
    if (y >= y_end) return;

    auto perf_row = [&](const char* lbl, double v, const char* unit,
                        int color = kPairWhite) {
        if (y >= y_end) return;
        mvprintw(y, 2, "%-22s", lbl);
        attron(COLOR_PAIR(color));
        char b[32];
        std::snprintf(b, sizeof(b), "%+10.4f%s", v, unit);
        mvaddstr(y, 26, b);
        attroff(COLOR_PAIR(color));
        ++y;
    };

    perf_row("Sharpe (rolling)", snap->perf.sharpe, "",
             snap->perf.sharpe > 1.0 ? kPairGreen
              : snap->perf.sharpe < 0.0 ? kPairRed : kPairWhite);
    perf_row("Win rate",         snap->perf.win_rate, "%",
             snap->perf.win_rate >= 55.0 ? kPairGreen
              : snap->perf.win_rate >= 45.0 ? kPairWhite : kPairYellow);
    perf_row("Avg markout",      snap->perf.avg_markout_bps, " bps",
             snap->perf.avg_markout_bps < 0.0 ? kPairGreen
              : snap->perf.avg_markout_bps > 2.0 ? kPairRed
              : snap->perf.avg_markout_bps > 0.5 ? kPairYellow : kPairWhite);

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

    // ── Recent events (fills the remaining vertical space) ──
    if (y >= y_end) return;
    label(y++, 2, "Recent events");

    int max_lines = y_end - y;
    if (max_lines <= 0) return;

    auto evs = data.recent_events_snapshot(static_cast<std::size_t>(max_lines));
    for (const auto& e : evs)
    {
        if (y >= y_end) break;
        std::string ts = fmt_hhmmss(e.ts);
        mvaddstr(y, 2, ts.c_str());
        attron(COLOR_PAIR(sev_pair(e.sev)));
        mvaddstr(y, 12, sev_text(e.sev));
        attroff(COLOR_PAIR(sev_pair(e.sev)));
        int maxw = width - 19;
        if (maxw < 1) maxw = 1;
        std::string msg = e.msg;
        if (static_cast<int>(msg.size()) > maxw) msg.resize(maxw);
        mvaddstr(y, 19, msg.c_str());
        ++y;
    }
}

} // namespace truetest::ui

#endif // HAS_RICH_TUI
