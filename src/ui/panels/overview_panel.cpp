#ifdef HAS_RICH_TUI

#include "overview_panel.h"

#include "../console_dashboard.h"
#include "../dashboard_snapshot.h"
#include "../tui_style.h"
#include "analytics/ascii_widgets.h"

#include <ncurses.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace truetest::ui {

namespace {

using Color = truetest::ui::Color;

Color pnl_color(double v)
{
    if (v > 0) return Color::Positive;
    if (v < 0) return Color::Negative;
    return Color::Neutral;
}

std::string fmt_int(std::uint64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu",
                  static_cast<unsigned long long>(v));
    return buf;
}

std::string fmt_money(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+10.2f", v);
    return buf;
}

std::string fmt_qty(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.6f", v);
    return buf;
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

}

void OverviewPanel::draw(int body_y0, int width, int height,
                         const ConsoleDashboard& data,
                         const dashboard_snapshot* snap)
{
    const auto& s = data.stats();
    const int x_label = 2;
    const int x_value = 18;
    const int x_label2 = 38;
    const int x_value2 = std::max(54, width / 2);

    // Decode atomics once.
    const std::uint64_t events = s.events_total.load(std::memory_order_relaxed);
    const std::uint64_t fills  = s.fills_total.load(std::memory_order_relaxed);
    const std::uint64_t trades = s.trades_total.load(std::memory_order_relaxed);
    const std::uint32_t open_ord = s.open_orders.load(std::memory_order_relaxed);
    const std::int64_t  pnl_fp4    = s.realized_pnl_fp4.load(std::memory_order_relaxed);
    const std::int64_t  unreal_fp4 = s.unrealized_pnl_fp4.load(std::memory_order_relaxed);
    const std::int64_t  pos_fp8    = s.position_qty_fp8.load(std::memory_order_relaxed);
    const std::int64_t  dd_fp4     = s.drawdown_fp4.load(std::memory_order_relaxed);
    const std::uint32_t wr_bps     = s.win_rate_bps.load(std::memory_order_relaxed);
    const std::int32_t  tox_fp2    = s.toxicity_bps_fp2.load(std::memory_order_relaxed);
    const std::uint32_t tox_n      = s.toxicity_samples.load(std::memory_order_relaxed);
    const std::uint64_t drops_log    = s.ring_drops_logging.load(std::memory_order_relaxed);
    const std::uint64_t drops_risk   = s.ring_drops_risk.load(std::memory_order_relaxed);
    const std::uint64_t drops_stats  = s.ring_drops_stats.load(std::memory_order_relaxed);
    const std::uint64_t drops_obs    = s.ring_drops_observer.load(std::memory_order_relaxed);
    const std::uint64_t drops_rs     = s.ring_drops_risk_stats.load(std::memory_order_relaxed);
    const std::uint64_t drops_mm     = s.ring_drops_mm.load(std::memory_order_relaxed);
    const std::uint64_t drops_total  = drops_log + drops_risk + drops_stats
                                     + drops_obs + drops_rs + drops_mm;
    const std::uint32_t bf_done  = s.backfill_done.load(std::memory_order_relaxed);
    const std::uint32_t bf_total = s.backfill_total.load(std::memory_order_relaxed);

    const double pnl   = static_cast<double>(pnl_fp4)    / 1e4;
    const double unrl  = static_cast<double>(unreal_fp4) / 1e4;
    const double pos   = static_cast<double>(pos_fp8)    / 1e8;
    const double dd    = static_cast<double>(dd_fp4)     / 1e2;
    const double tox   = static_cast<double>(tox_fp2)    / 1e2;

    int y = body_y0;
    if (y >= body_y0 + height) return;

    // ── Key Money Metrics ──
    // These 5 numbers are what most traders look at first and most often.

    // Equity
    draw_label(y, x_label, "Equity");
    set_color_bold(Color::Neutral);
    double equity_val = snap ? snap->equity : 0.0;
    mvaddstr(y, x_value, fmt_money(equity_val).c_str());
    unset_color_bold(Color::Neutral);
    ++y;

    // Realized PnL
    draw_label(y, x_label, "Realized");
    set_color_bold(pnl_color(pnl));
    mvaddstr(y, x_value, fmt_money(pnl).c_str());
    unset_color_bold(pnl_color(pnl));
    ++y;

    // Unrealized PnL
    draw_label(y, x_label, "Unrealized");
    set_color_bold(pnl_color(unrl));
    mvaddstr(y, x_value, fmt_money(unrl).c_str());
    unset_color_bold(pnl_color(unrl));
    ++y;

    // TOTAL PnL - the single most important number (make it stand out)
    double total_pnl = pnl + unrl;
    draw_label(y, x_label, "TOTAL PnL");
    set_color_bold(pnl_color(total_pnl));
    mvaddstr(y, x_value, fmt_money(total_pnl).c_str());
    unset_color_bold(pnl_color(total_pnl));
    ++y;

    // Current Position
    draw_label(y, x_label, "Position");
    Color pos_col = (pos > 0) ? Color::Positive : (pos < 0 ? Color::Negative : Color::Neutral);
    set_color_bold(pos_col);
    mvaddstr(y, x_value, fmt_qty(pos).c_str());
    unset_color_bold(pos_col);
    ++y;

    if (y < body_y0 + height) mvhline(y++, 1, ACS_HLINE, width - 2);

    if (bf_total > 0)
    {
        draw_label(y, x_label, "Backfill");
        char b[32];
        std::snprintf(b, sizeof(b), "%u/%u %s", bf_done, bf_total,
                      bf_done >= bf_total ? "done" : "...");
        mvaddstr(y, x_value, b);
        ++y;
    }

    // Spacer
    if (y < body_y0 + height) mvhline(y++, 1, ACS_HLINE, width - 2);

    // ========== ACTIVITY SECTION ==========
    // Activity
    draw_label(y, x_label, "Events");
    mvaddstr(y, x_value, fmt_int(events).c_str());
    draw_label(y, x_label2, "Fills");
    mvaddstr(y, x_value2, fmt_int(fills).c_str());
    ++y;

    draw_label(y, x_label, "Trades");
    mvaddstr(y, x_value, fmt_int(trades).c_str());
    draw_label(y, x_label2, "Open Orders");
    mvaddstr(y, x_value2, fmt_int(open_ord).c_str());
    ++y;

    if (y < body_y0 + height) mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Performance ──
    draw_label(y, x_label, "Win Rate");
    if (trades > 0)
    {
        int pct = static_cast<int>(wr_bps / 100);
        Color c = (pct >= 55) ? Color::Positive : (pct >= 45 ? Color::Neutral : Color::Warning);
        set_color_bold(c);
        char b[16];
        std::snprintf(b, sizeof(b), "%d%%", pct);
        mvaddstr(y, x_value, b);
        unset_color_bold(c);
    }
    else mvaddstr(y, x_value, "-");

    draw_label(y, x_label2, "Toxicity");
    if (tox_n > 0)
    {
        Color c = (tox > 2.0) ? Color::Danger : (tox > 0.5 ? Color::Warning : (tox < 0.0 ? Color::Positive : Color::Neutral));
        set_color(c);
        char b[32];
        std::snprintf(b, sizeof(b), "%+5.2f bps", tox);
        mvaddstr(y, x_value2, b);
        unset_color(c);
    }
    else mvaddstr(y, x_value2, "-");
    ++y;

    label(y, x_label, "Drawdown");
    Color dd_col = (dd <= -5.0) ? Color::Danger : (dd <= -1.0 ? Color::Warning : Color::Neutral);
    set_color(dd_col);
    char dd_buf[16];
    std::snprintf(dd_buf, sizeof(dd_buf), "%+6.2f%%", dd);
    mvaddstr(y, x_value, dd_buf);
    unset_color(dd_col);
    ++y;

    // Row: inventory bar (current exposure vs limit) + cash
    if (snap)
    {
        label(y, x_label, "Inventory");
        const double exp_lim = snap->risk.exposure_limit;
        if (exp_lim > 0.0)
        {
            const double frac = std::min(1.0, snap->risk.exposure / exp_lim);
            const int bar_w = 24;
            const int filled = static_cast<int>(std::lround(frac * bar_w));
            int p = (frac >= 0.85) ? kPairRed
                  : (frac >= 0.6)  ? kPairYellow
                  : kPairGreen;
            mvaddch(y, x_value, '[');
            attron(COLOR_PAIR(p));
            for (int i = 0; i < filled; ++i)
                mvaddch(y, x_value + 1 + i, ACS_CKBOARD);
            attroff(COLOR_PAIR(p));
            for (int i = filled; i < bar_w; ++i)
                mvaddch(y, x_value + 1 + i, '.');
            mvaddch(y, x_value + 1 + bar_w, ']');
            char b[16];
            std::snprintf(b, sizeof(b), " %3.0f%%", frac * 100.0);
            mvaddstr(y, x_value + 2 + bar_w, b);
        }
        else
        {
            attron(A_DIM);
            mvaddstr(y, x_value, "(no exposure limit set)");
            attroff(A_DIM);
        }
        label(y, x_label2 + 28, "Cash");
        char cb[24];
        std::snprintf(cb, sizeof(cb), "%10.2f", snap->cash);
        mvaddstr(y, x_label2 + 36, cb);
        ++y;
    }

    // Row: per-ring drops detail (only show if any) - status bar shows total.
    if (drops_total > 0)
    {
        label(y, x_label, "Drops");
        char b[96];
        std::snprintf(b, sizeof(b),
                      "log:%llu  risk:%llu  stats:%llu  obs:%llu  rs:%llu  mm:%llu",
                      static_cast<unsigned long long>(drops_log),
                      static_cast<unsigned long long>(drops_risk),
                      static_cast<unsigned long long>(drops_stats),
                      static_cast<unsigned long long>(drops_obs),
                      static_cast<unsigned long long>(drops_rs),
                      static_cast<unsigned long long>(drops_mm));
        mvaddstr(y, x_value, b);
        ++y;
    }

    // Spacer
    if (y < body_y0 + height) mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Trend strip ─────────────────────────────────────────────────
    // Three sparkline rows that turn the previously-empty bottom half
    // into an at-a-glance health view. Sourced from the snapshot's
    // trend_view (equity + drawdown) and ConsoleDashboard's rate ring.
    if (y < body_y0 + height)
    {
        const int spark_x   = x_label + 12;            // leave label gutter
        const int spark_w   = std::max(8, width - spark_x - 22);
        const int value_x   = spark_x + spark_w + 2;

        auto draw_spark_row = [&](const char* lbl,
                                  const std::vector<double>& series,
                                  const char* value_str,
                                  int value_pair)
        {
            if (y >= body_y0 + height) return;
            label(y, x_label, lbl);
            if (series.size() >= 2)
            {
                auto s = tt::ascii::sparkline(series,
                            static_cast<std::size_t>(spark_w));
                mvaddstr(y, spark_x, s.c_str());
            }
            else
            {
                attron(A_DIM);
                for (int i = 0; i < spark_w; ++i) mvaddch(y, spark_x + i, '.');
                attroff(A_DIM);
            }
            attron(COLOR_PAIR(value_pair));
            mvaddstr(y, value_x, value_str);
            attroff(COLOR_PAIR(value_pair));
            ++y;
        };

        char eq_lbl[32];
        std::snprintf(eq_lbl, sizeof(eq_lbl), "%+6.2f%%",
                      snap ? snap->trend.equity_change_pct : 0.0);
        const int eq_pair =
            !snap ? kPairWhite
            : snap->trend.equity_change_pct > 0 ? kPairGreen
            : snap->trend.equity_change_pct < 0 ? kPairRed
            : kPairWhite;
        draw_spark_row("Equity",
                       snap ? snap->trend.equity_tail : std::vector<double>{},
                       eq_lbl, eq_pair);

        char dd_lbl[32];
        const double dd_now = snap ? snap->trend.drawdown_now_pct : 0.0;
        std::snprintf(dd_lbl, sizeof(dd_lbl), "-%5.2f%%", dd_now);
        const int dd_pair = (dd_now >= 5.0) ? kPairRed
                          : (dd_now >= 1.0) ? kPairYellow
                          : kPairWhite;
        draw_spark_row("Drawdown",
                       snap ? snap->trend.drawdown_tail : std::vector<double>{},
                       dd_lbl, dd_pair);

        auto rate_series = data.rate_tail(60);
        char rt_lbl[32];
        std::snprintf(rt_lbl, sizeof(rt_lbl), "%6.0f ev/s", data.rate_ema());
        draw_spark_row("Event rate", rate_series, rt_lbl, kPairCyan);
    }

    if (y < body_y0 + height) mvhline(y++, 1, ACS_HLINE, width - 2);

    // Recent events. Use the public seqlock snapshot accessor.
    label(y, x_label, "Recent events");
    ++y;

    int max_lines = (body_y0 + height) - y;
    if (max_lines < 0) max_lines = 0;
    auto evs = data.recent_events_snapshot(static_cast<std::size_t>(max_lines));
    for (const auto& e : evs)
    {
        if (y >= body_y0 + height) break;
        std::string ts = fmt_hhmmss(e.ts);
        mvaddstr(y, x_label, ts.c_str());
        attron(COLOR_PAIR(sev_pair(e.sev)));
        mvaddstr(y, x_label + 10, sev_text(e.sev));
        attroff(COLOR_PAIR(sev_pair(e.sev)));
        // truncate message to fit
        int maxw = width - (x_label + 17);
        if (maxw < 1) maxw = 1;
        std::string msg = e.msg;
        if (static_cast<int>(msg.size()) > maxw) msg.resize(maxw);
        mvaddstr(y, x_label + 17, msg.c_str());
        ++y;
    }
}

}

#endif // HAS_RICH_TUI
