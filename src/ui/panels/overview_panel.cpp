#ifdef HAS_RICH_TUI

#include "overview_panel.h"

#include "../console_dashboard.h"
#include "../dashboard_snapshot.h"
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

constexpr int kPairGreen  = 1;
constexpr int kPairRed    = 2;
constexpr int kPairYellow = 3;
constexpr int kPairCyan   = 4;
constexpr int kPairWhite  = 5;

void label(int y, int x, const char* text)
{
    attron(COLOR_PAIR(kPairCyan));
    mvaddstr(y, x, text);
    attroff(COLOR_PAIR(kPairCyan));
}

int pnl_pair(double v)
{
    if (v > 0) return kPairGreen;
    if (v < 0) return kPairRed;
    return kPairWhite;
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

std::string fmt_price(double v, int dec = 2)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
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

const char* state_text(connection_state s)
{
    switch (s)
    {
        case connection_state::idle:         return "idle";
        case connection_state::backfill:     return "backfill";
        case connection_state::waiting:      return "waiting";
        case connection_state::live:         return "live";
        case connection_state::reconnecting: return "reconnecting";
        case connection_state::halted:       return "halted";
        case connection_state::closed:       return "closed";
    }
    return "?";
}

int state_pair(connection_state s)
{
    switch (s)
    {
        case connection_state::live:         return kPairGreen;
        case connection_state::halted:       return kPairRed;
        case connection_state::reconnecting: return kPairYellow;
        case connection_state::backfill:     return kPairYellow;
        case connection_state::waiting:      return kPairYellow;
        default:                             return kPairWhite;
    }
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

} // namespace

void OverviewPanel::draw(int body_y0, int width, int height,
                         const ConsoleDashboard& data,
                         const dashboard_snapshot* snap)
{
    const auto& s = data.stats();
    const int x_label = 2;
    const int x_value = 18;
    const int x_label2 = 38;
    const int x_value2 = 54;

    // Decode atomics once.
    const auto state    = static_cast<connection_state>(s.state.load(std::memory_order_acquire));
    const std::uint64_t events = s.events_total.load(std::memory_order_relaxed);
    const std::uint64_t fills  = s.fills_total.load(std::memory_order_relaxed);
    const std::uint64_t trades = s.trades_total.load(std::memory_order_relaxed);
    const std::int64_t  last_fp8 = s.last_price_fp8.load(std::memory_order_relaxed);
    const std::int64_t  bid_fp8  = s.best_bid_fp8.load(std::memory_order_relaxed);
    const std::int64_t  ask_fp8  = s.best_ask_fp8.load(std::memory_order_relaxed);
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
    const bool halted = s.halt_flag.load(std::memory_order_acquire);
    const std::uint32_t bf_done  = s.backfill_done.load(std::memory_order_relaxed);
    const std::uint32_t bf_total = s.backfill_total.load(std::memory_order_relaxed);

    // Decode fp prices.
    const double last  = (last_fp8 < 0) ? 0.0 : static_cast<double>(last_fp8) / 1e8;
    const double bid   = (bid_fp8  < 0) ? 0.0 : static_cast<double>(bid_fp8)  / 1e8;
    const double ask   = (ask_fp8  < 0) ? 0.0 : static_cast<double>(ask_fp8)  / 1e8;
    const double pnl   = static_cast<double>(pnl_fp4)    / 1e4;
    const double unrl  = static_cast<double>(unreal_fp4) / 1e4;
    const double pos   = static_cast<double>(pos_fp8)    / 1e8;
    const double dd    = static_cast<double>(dd_fp4)     / 1e2;
    const double tox   = static_cast<double>(tox_fp2)    / 1e2;

    int y = body_y0;
    if (y >= body_y0 + height) return;

    // Row: bid · ask · spread (last is in status bar) + rate
    label(y, x_label, "Bid/Ask");
    if (bid_fp8 > 0 && ask_fp8 > 0)
    {
        char b[64];
        const double mid = (bid + ask) * 0.5;
        const double bps = mid > 0 ? (ask - bid) / mid * 1e4 : 0.0;
        std::snprintf(b, sizeof(b), "%.2f / %.2f  (%.1fbp)", bid, ask, bps);
        mvaddstr(y, x_value, b);
    }
    else mvaddstr(y, x_value, "—");
    label(y, x_label2, "Rate");
    {
        char rb[32];
        std::snprintf(rb, sizeof(rb), "%6.1f ev/s", data.rate_ema());
        mvaddstr(y, x_value2, rb);
    }
    ++y;

    if (bf_total > 0)
    {
        label(y, x_label, "Backfill");
        char b[32];
        std::snprintf(b, sizeof(b), "%u/%u %s", bf_done, bf_total,
                      bf_done >= bf_total ? "done" : "...");
        mvaddstr(y, x_value, b);
        ++y;
    }

    // Spacer
    if (y < body_y0 + height) mvhline(y++, 1, ACS_HLINE, width - 2);

    // Row: events / fills / trades
    label(y, x_label, "Events");
    mvaddstr(y, x_value, fmt_int(events).c_str());
    label(y, x_label2, "Fills");
    mvaddstr(y, x_value2, fmt_int(fills).c_str());
    ++y;
    label(y, x_label, "Round-trips");
    mvaddstr(y, x_value, fmt_int(trades).c_str());
    label(y, x_label2, "Open ord");
    mvaddstr(y, x_value2, fmt_int(open_ord).c_str());
    ++y;

    // Row: realized / drawdown
    label(y, x_label, "Realized");
    attron(COLOR_PAIR(pnl_pair(pnl)));
    mvaddstr(y, x_value, fmt_money(pnl).c_str());
    attroff(COLOR_PAIR(pnl_pair(pnl)));
    label(y, x_label2, "Drawdown");
    {
        int p = dd <= -5.0 ? kPairRed : (dd <= -1.0 ? kPairYellow : kPairWhite);
        attron(COLOR_PAIR(p));
        char b[16];
        std::snprintf(b, sizeof(b), "%+6.2f%%", dd);
        mvaddstr(y, x_value2, b);
        attroff(COLOR_PAIR(p));
    }
    ++y;

    // Row: unrealized / position
    label(y, x_label, "Unrealized");
    attron(COLOR_PAIR(pnl_pair(unrl)));
    mvaddstr(y, x_value, fmt_money(unrl).c_str());
    attroff(COLOR_PAIR(pnl_pair(unrl)));
    label(y, x_label2, "Position");
    {
        int p = pos > 0 ? kPairGreen : (pos < 0 ? kPairRed : kPairWhite);
        attron(COLOR_PAIR(p));
        mvaddstr(y, x_value2, fmt_qty(pos).c_str());
        attroff(COLOR_PAIR(p));
    }
    ++y;

    // Row: toxicity / win rate
    label(y, x_label, "Toxicity");
    if (tox_n > 0)
    {
        int p = tox >  2.0 ? kPairRed
              : tox >  0.5 ? kPairYellow
              : tox <  0.0 ? kPairGreen
              : kPairWhite;
        attron(COLOR_PAIR(p));
        char b[32];
        std::snprintf(b, sizeof(b), "%+5.2f bps (n=%u)", tox, tox_n);
        mvaddstr(y, x_value, b);
        attroff(COLOR_PAIR(p));
    }
    else mvaddstr(y, x_value, "—");
    label(y, x_label2, "Win rate");
    if (trades > 0)
    {
        int pct = static_cast<int>(wr_bps / 100);
        int p = pct >= 55 ? kPairGreen : (pct >= 45 ? kPairWhite : kPairYellow);
        attron(COLOR_PAIR(p));
        char b[8];
        std::snprintf(b, sizeof(b), "%d%%", pct);
        mvaddstr(y, x_value2, b);
        attroff(COLOR_PAIR(p));
    }
    else mvaddstr(y, x_value2, "—");
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

    // Row: per-ring drops detail (only show if any) — status bar shows total.
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

} // namespace truetest::ui

#endif // HAS_RICH_TUI
