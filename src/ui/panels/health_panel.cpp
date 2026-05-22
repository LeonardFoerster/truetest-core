#ifdef HAS_RICH_TUI

#include "health_panel.h"

#include "../console_dashboard.h"
#include "../dashboard_snapshot.h"
#include "../tui_style.h"

#include <ncurses.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace truetest::ui {

namespace {

const char* state_text(int s)
{
    switch (static_cast<connection_state>(s)) {
        case connection_state::idle:         return "idle";
        case connection_state::backfill:     return "backfill";
        case connection_state::waiting:      return "waiting";
        case connection_state::live:         return "LIVE";
        case connection_state::reconnecting: return "reconnecting";
        case connection_state::halted:       return "HALTED";
        case connection_state::closed:       return "closed";
    }
    return "?";
}

int state_pair(int s)
{
    switch (static_cast<connection_state>(s)) {
        case connection_state::live:         return kPairGreen;
        case connection_state::halted:       return kPairRed;
        case connection_state::reconnecting:
        case connection_state::backfill:
        case connection_state::waiting:      return kPairYellow;
        default:                             return kPairWhite;
    }
}

}

void HealthPanel::draw(int body_y0, int width, int height,
                       const ConsoleDashboard& data,
                       const dashboard_snapshot* snap)
{
    if (!snap)
    {
        mvaddstr(body_y0, 2, "(no snapshot yet — engine warming up)");
        return;
    }

    int y = body_y0;
    const int y_end = body_y0 + height;
    const int xL = 2, xLv = 22;
    const int xR = std::max(50, width / 2);
    const int xRv = xR + 22;
    (void)width;

    // ── Provider state ──
    label(y, xL, "Provider");
    {
        const std::string nm = snap->health.provider_present
            ? snap->health.provider_name : std::string("(none)");
        mvprintw(y, xLv, "%-22.22s", nm.c_str());
    }
    label(y, xR, "State");
    {
        int p = state_pair(snap->health.provider_state);
        attron(COLOR_PAIR(p) | A_BOLD);
        mvaddstr(y, xRv, state_text(snap->health.provider_state));
        attroff(COLOR_PAIR(p) | A_BOLD);
    }
    ++y;
    if (y >= y_end) return;

    if (y < y_end) mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Latency (tick → trade) ──
    label(y++, xL, "Latency  (tick → trade)");
    if (y >= y_end) return;
    {
        const auto& h = snap->health;
        if (h.tick_to_trade_samples > 0)
        {
            char b[64];
            label(y, xL, "avg");
            std::snprintf(b, sizeof(b), "%8.2f µs", h.avg_tick_to_trade_us);
            mvaddstr(y, xLv, b);
            label(y, xR, "samples");
            mvprintw(y, xRv, "%zu", h.tick_to_trade_samples);
            ++y;

            if (y >= y_end) return;
            label(y, xL, "min");
            std::snprintf(b, sizeof(b), "%8.2f µs", h.min_tick_to_trade_us);
            mvaddstr(y, xLv, b);
            label(y, xR, "max");
            int p = (h.max_tick_to_trade_us > 50000) ? kPairRed
                  : (h.max_tick_to_trade_us > 10000) ? kPairYellow : kPairWhite;
            attron(COLOR_PAIR(p));
            std::snprintf(b, sizeof(b), "%8.2f µs", h.max_tick_to_trade_us);
            mvaddstr(y, xRv, b);
            attroff(COLOR_PAIR(p));
            ++y;
        }
        else
        {
            attron(A_DIM);
            mvaddstr(y++, xL + 2, "(no fills yet)");
            attroff(A_DIM);
        }
    }

    if (y < y_end) mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Throughput (sourced live from ConsoleDashboard) ──
    label(y++, xL, "Throughput");
    if (y >= y_end) return;
    {
        const auto& s = data.stats();
        const std::uint64_t events = s.events_total.load(std::memory_order_relaxed);
        const std::uint64_t fills  = s.fills_total.load(std::memory_order_relaxed);
        const std::uint64_t trades = s.trades_total.load(std::memory_order_relaxed);

        char b[64];
        label(y, xL, "events");
        std::snprintf(b, sizeof(b), "%llu", (unsigned long long)events);
        mvaddstr(y, xLv, b);
        label(y, xR, "rate");
        std::snprintf(b, sizeof(b), "%9.1f ev/s", data.rate_ema());
        mvaddstr(y, xRv, b);
        ++y;
        if (y >= y_end) return;

        label(y, xL, "fills");
        std::snprintf(b, sizeof(b), "%llu", (unsigned long long)fills);
        mvaddstr(y, xLv, b);
        label(y, xR, "round-trips");
        std::snprintf(b, sizeof(b), "%llu", (unsigned long long)trades);
        mvaddstr(y, xRv, b);
        ++y;
    }

    if (y < y_end) mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Ring drops (per-worker breakdown) ──
    label(y++, xL, "Ring drops (back-pressure)");
    if (y >= y_end) return;
    {
        const auto& s = data.stats();
        struct ring { const char* lbl; std::uint64_t n; };
        ring rings[] = {
            {"logging",    s.ring_drops_logging.load(std::memory_order_relaxed)},
            {"risk",       s.ring_drops_risk.load(std::memory_order_relaxed)},
            {"stats",      s.ring_drops_stats.load(std::memory_order_relaxed)},
            {"observer",   s.ring_drops_observer.load(std::memory_order_relaxed)},
            {"risk_stats", s.ring_drops_risk_stats.load(std::memory_order_relaxed)},
            {"mm",         s.ring_drops_mm.load(std::memory_order_relaxed)},
        };
        std::uint64_t total = 0;
        for (auto& r : rings) total += r.n;

        for (std::size_t i = 0; i < sizeof(rings)/sizeof(rings[0]) && y < y_end; ++i)
        {
            const auto& r = rings[i];
            const int col_x = (i < 3) ? xL : xR;
            const int val_x = (i < 3) ? xLv : xRv;
            const int row_y = y + static_cast<int>(i % 3);
            label(row_y, col_x, r.lbl);
            int p = (r.n > 0) ? kPairRed : kPairWhite;
            attron(COLOR_PAIR(p));
            mvprintw(row_y, val_x, "%llu", (unsigned long long)r.n);
            attroff(COLOR_PAIR(p));
        }
        y += 3;
        if (y < y_end)
        {
            attron(A_DIM);
            char b[48];
            std::snprintf(b, sizeof(b), "  total: %llu", (unsigned long long)total);
            mvaddstr(y++, xL, b);
            attroff(A_DIM);
        }
    }

    if (y < y_end) mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Footer hint about Health gaps ──
    if (y < y_end)
    {
        attron(A_DIM);
        mvaddstr(y, xL,
            "Note: WS reconnect count and message-age tracking are not yet wired");
        attroff(A_DIM);
    }
}

}

#endif // HAS_RICH_TUI
