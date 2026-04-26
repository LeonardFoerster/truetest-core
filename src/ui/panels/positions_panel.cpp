#ifdef HAS_RICH_TUI

#include "positions_panel.h"

#include "../console_dashboard.h"
#include "../dashboard_snapshot.h"

#include <ncurses.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

int signed_pair(double v)
{
    if (v > 0) return kPairGreen;
    if (v < 0) return kPairRed;
    return kPairWhite;
}

std::string fmt_age(std::int64_t s)
{
    char buf[16];
    if (s < 60)        std::snprintf(buf, sizeof(buf), "%llds",
                                     static_cast<long long>(s));
    else if (s < 3600) std::snprintf(buf, sizeof(buf), "%lldm%02llds",
                                     static_cast<long long>(s / 60),
                                     static_cast<long long>(s % 60));
    else               std::snprintf(buf, sizeof(buf), "%lldh%02lldm",
                                     static_cast<long long>(s / 3600),
                                     static_cast<long long>((s % 3600) / 60));
    return buf;
}

} // namespace

void PositionsPanel::draw(int body_y0, int width, int height,
                          const ConsoleDashboard& /*data*/,
                          const dashboard_snapshot* snap)
{
    if (!snap)
    {
        mvaddstr(body_y0, 2, "(no snapshot yet — engine warming up)");
        return;
    }

    int y = body_y0;
    int y_end = body_y0 + height;

    // Account header
    label(y, 2,  "Cash");
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%12.2f", snap->cash);
        mvaddstr(y, 8, b);
    }
    label(y, 26, "Equity");
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%12.2f", snap->equity);
        mvaddstr(y, 34, b);
    }
    label(y, 52, "uPnL");
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%+10.2f", snap->unrealized_pnl);
        attron(COLOR_PAIR(signed_pair(snap->unrealized_pnl)));
        mvaddstr(y, 58, b);
        attroff(COLOR_PAIR(signed_pair(snap->unrealized_pnl)));
    }
    ++y;

    if (y < y_end) mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Positions table (netted, per-symbol) ──
    label(y++, 2, "Positions (netted)");
    if (y >= y_end) return;

    // Column header
    attron(A_DIM);
    mvprintw(y, 2,  "%-10s", "Symbol");
    mvprintw(y, 14, "%14s", "Qty");
    mvprintw(y, 30, "%14s", "Avg Entry");
    mvprintw(y, 46, "%14s", "Mark");
    mvprintw(y, 62, "%14s", "uPnL");
    attroff(A_DIM);
    ++y;

    if (snap->positions.empty())
    {
        attron(A_DIM);
        mvaddstr(y++, 4, "(no open positions)");
        attroff(A_DIM);
    }
    else
    {
        for (const auto& p : snap->positions)
        {
            if (y >= y_end) break;
            mvprintw(y, 2, "%-10.10s", p.symbol.c_str());
            int qpair = signed_pair(p.qty);
            attron(COLOR_PAIR(qpair));
            mvprintw(y, 14, "%+14.6f", p.qty);
            attroff(COLOR_PAIR(qpair));
            mvprintw(y, 30, "%14.4f", p.avg_entry);
            if (p.mark > 0.0) mvprintw(y, 46, "%14.4f", p.mark);
            else              mvprintw(y, 46, "%14s", "—");
            int upair = signed_pair(p.unrealized);
            attron(COLOR_PAIR(upair));
            mvprintw(y, 62, "%+14.4f", p.unrealized);
            attroff(COLOR_PAIR(upair));
            ++y;
        }
    }

    if (y >= y_end) return;
    mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Lots table (per-strategy attribution) ──
    label(y++, 2, "Open lots (per strategy)");
    if (y >= y_end) return;

    attron(A_DIM);
    mvprintw(y, 2,  "%-8s", "Side");
    mvprintw(y, 10, "%-12s", "Symbol");
    mvprintw(y, 22, "%-16s", "Strategy");
    mvprintw(y, 38, "%14s", "Qty open");
    mvprintw(y, 54, "%14s", "Entry");
    mvprintw(y, 68, "%10s", "Age");
    attroff(A_DIM);
    ++y;

    if (snap->lots.empty())
    {
        attron(A_DIM);
        mvaddstr(y++, 4, "(no open lots)");
        attroff(A_DIM);
    }
    else
    {
        for (const auto& l : snap->lots)
        {
            if (y >= y_end) break;
            int spair = (l.side == 'L') ? kPairGreen
                       : (l.side == 'S') ? kPairRed : kPairWhite;
            attron(COLOR_PAIR(spair) | A_BOLD);
            mvaddstr(y, 2, l.side == 'L' ? "LONG"
                          : l.side == 'S' ? "SHORT" : "?");
            attroff(COLOR_PAIR(spair) | A_BOLD);
            mvprintw(y, 10, "%-12.12s", l.symbol.c_str());
            mvprintw(y, 22, "%-16.16s",
                     l.strategy_name.empty() ? "—" : l.strategy_name.c_str());
            mvprintw(y, 38, "%14.6f", l.qty_open);
            mvprintw(y, 54, "%14.4f", l.entry_price);
            mvprintw(y, 68, "%10s", fmt_age(l.age_seconds).c_str());
            ++y;
        }
    }
}

} // namespace truetest::ui

#endif // HAS_RICH_TUI
