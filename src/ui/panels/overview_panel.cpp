#ifdef HAS_RICH_TUI

#include "overview_panel.h"

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
                         const dashboard_snapshot* /*snap*/)
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

    // Row 1: state + uptime
    label(y, x_label, "State");
    attron(COLOR_PAIR(state_pair(state)) | A_BOLD);
    mvprintw(y, x_value, "%-12s", state_text(state));
    attroff(COLOR_PAIR(state_pair(state)) | A_BOLD);
    {
        // uptime since the dashboard process started; we don't have direct
        // access to start_time_, so use the most recent seen-event timestamp
        // delta as a proxy when present; otherwise leave blank.
        // Simpler: pull rate from public accessor for the right-side cell.
        char rb[32];
        std::snprintf(rb, sizeof(rb), "%6.1f ev/s", data.rate_ema());
        label(y, x_label2, "Rate");
        mvaddstr(y, x_value2, rb);
    }
    ++y;

    // Row 2: last + spread
    label(y, x_label, "Last");
    if (last_fp8 >= 0) mvaddstr(y, x_value, fmt_price(last).c_str());
    else               mvaddstr(y, x_value, "—");
    label(y, x_label2, "Spread");
    if (bid_fp8 > 0 && ask_fp8 > 0)
    {
        double mid = (bid + ask) * 0.5;
        double bps = mid > 0 ? (ask - bid) / mid * 1e4 : 0.0;
        char b[32];
        std::snprintf(b, sizeof(b), "%5.2f bps", bps);
        mvaddstr(y, x_value2, b);
    }
    else mvaddstr(y, x_value2, "—");
    ++y;

    // Row 3: bid / ask
    label(y, x_label, "Bid");
    mvaddstr(y, x_value, bid_fp8 > 0 ? fmt_price(bid).c_str() : "—");
    label(y, x_label2, "Ask");
    mvaddstr(y, x_value2, ask_fp8 > 0 ? fmt_price(ask).c_str() : "—");
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

    // Row: risk
    label(y, x_label, "Halt");
    {
        int p = halted ? kPairRed : kPairGreen;
        attron(COLOR_PAIR(p) | A_BOLD);
        mvaddstr(y, x_value, halted ? "YES" : "no");
        attroff(COLOR_PAIR(p) | A_BOLD);
    }
    label(y, x_label2, "Drops");
    {
        int p = drops_total > 0 ? kPairRed : kPairWhite;
        attron(COLOR_PAIR(p));
        mvaddstr(y, x_value2, fmt_int(drops_total).c_str());
        attroff(COLOR_PAIR(p));
    }
    ++y;

    // Row: per-ring drops detail (only show if any)
    if (drops_total > 0)
    {
        label(y, x_label, "Rings");
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

    // Recent events. Use the public seqlock snapshot accessor.
    label(y, x_label, "Recent events");
    ++y;

    int max_lines = (body_y0 + height) - y - 1;
    if (max_lines < 0) max_lines = 0;
    int want = (std::min)(max_lines, 8);
    auto evs = data.recent_events_snapshot(static_cast<std::size_t>(want));
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
