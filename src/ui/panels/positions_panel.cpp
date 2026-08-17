#ifdef HAS_RICH_TUI

#include "positions_panel.h"

#include "../console_dashboard.h"
#include "../dashboard_snapshot.h"
#include "../tui_style.h"

#include <ncurses.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace truetest::ui {

namespace {

int signed_pair(double v)
{
    if (v > 0) return kPairGreen;
    if (v < 0) return kPairRed;
    return kPairWhite;
}

std::string fmt_age(std::int64_t s)
{
    char buf[32];
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

}

void PositionsPanel::draw(int body_y0, int width, int height,
                          const ConsoleDashboard& /*data*/,
                          const dashboard_snapshot* snap)
{
    if (!snap)
    {
        mvaddstr(body_y0, 2, "(no snapshot yet - engine warming up)");
        return;
    }

    cursor_max_ = static_cast<int>(snap->positions.size());
    if (cursor_row_ >= cursor_max_ && cursor_max_ > 0)
        cursor_row_ = cursor_max_ - 1;
    if (cursor_row_ < 0)
        cursor_row_ = 0;

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

    // Layout: on wide terminals (≥ 160 cols) render the two tables
    // side-by-side. Otherwise stack them vertically as before. Wide
    // mode lets each table use the full vertical space and removes
    // the awkward 50/50 split when one table dominates.
    const bool wide = (width >= 160);
    const int  lot_x_offset = wide ? 80 : 0;   // shift lots to right half
    const int remaining = y_end - y;
    const int positions_end = wide ? y_end : (y + remaining / 2);
    const int lots_end      = y_end;
    const int lots_y_start  = wide ? y : positions_end;

    // ── Positions table (netted, per-symbol) ──
    label(y++, 2, "Positions (netted)");
    if (y < positions_end)
    {
        attron(A_DIM);
        mvprintw(y, 2,  "%-9s",  "Symbol");
        mvprintw(y, 12, "%12s",  "Qty");
        mvprintw(y, 25, "%10s",  "Entry");
        mvprintw(y, 36, "%10s",  "Mark");
        mvprintw(y, 47, "%8s",   "Δ%");
        mvprintw(y, 56, "%12s",  "uPnL");
        mvprintw(y, 69, "%8s",   "uPnL%");
        attroff(A_DIM);
        ++y;
    }

    std::vector<const dashboard_snapshot::position_row*> visible_pos;
    for (const auto& p : snap->positions)
    {
        if (filter_.empty() || p.symbol.find(filter_) != std::string::npos)
            visible_pos.push_back(&p);
    }

    cursor_max_ = static_cast<int>(visible_pos.size());
    if (cursor_row_ >= cursor_max_ && cursor_max_ > 0)
        cursor_row_ = cursor_max_ - 1;
    if (cursor_row_ < 0)
        cursor_row_ = 0;

    if (visible_pos.empty() && y < positions_end)
    {
        attron(A_DIM);
        mvaddstr(y++, 4, "(no open positions)");
        attroff(A_DIM);
    }
    else
    {
        std::size_t shown = 0;
        for (std::size_t i = 0; i < visible_pos.size(); ++i)
        {
            const auto& p = *visible_pos[i];

            if (y >= positions_end - 1 && shown < visible_pos.size())
            {
                if (visible_pos.size() - shown > 0)
                {
                    attron(A_DIM);
                    mvprintw(y, 4, "+ %zu more …", visible_pos.size() - shown);
                    attroff(A_DIM);
                }
                break;
            }
            if (y >= positions_end) break;

            const bool selected = (static_cast<int>(i) == cursor_row_);
            if (selected) attron(A_REVERSE);

            mvprintw(y, 2, "%-9.9s", p.symbol.c_str());
            int qpair = signed_pair(p.qty);
            attron(COLOR_PAIR(qpair));
            mvprintw(y, 12, "%+12.6f", p.qty);
            attroff(COLOR_PAIR(qpair));
            mvprintw(y, 25, "%10.4f", p.avg_entry);
            if (p.mark > 0.0) mvprintw(y, 36, "%10.4f", p.mark);
            else              mvprintw(y, 36, "%10s", "-");

            // Δ% - price drift from entry.
            if (p.mark > 0.0 && p.avg_entry > 0.0)
            {
                const double drift = (p.mark - p.avg_entry) / p.avg_entry * 100.0;
                int dp = signed_pair(drift);
                attron(COLOR_PAIR(dp));
                mvprintw(y, 47, "%+7.2f%%", drift);
                attroff(COLOR_PAIR(dp));
            }
            else mvprintw(y, 47, "%8s", "-");

            int upair = signed_pair(p.unrealized);
            attron(COLOR_PAIR(upair));
            mvprintw(y, 56, "%+12.4f", p.unrealized);
            attroff(COLOR_PAIR(upair));

            // uPnL% - unrealized as fraction of cost basis.
            if (std::abs(p.qty) > 0.0 && p.avg_entry > 0.0)
            {
                const double basis = std::abs(p.qty) * p.avg_entry;
                const double upct  = (basis > 0.0) ? p.unrealized / basis * 100.0 : 0.0;
                int up = signed_pair(upct);
                attron(COLOR_PAIR(up));
                mvprintw(y, 69, "%+7.2f%%", upct);
                attroff(COLOR_PAIR(up));
            }
            else mvprintw(y, 69, "%8s", "-");

            ++y; ++shown;
        }
    }

    // Cursor hint (consistent style)
    if (cursor_max_ > 0 && y < positions_end)
    {
        attron(A_DIM);
        mvprintw(y, 4, "j/k: navigate (cursor on positions)");
        attroff(A_DIM);
    }

    // Lots section. In wide mode it lives in the right half (x_offset);
    // in narrow mode it lives below positions, separated by a divider.
    y = lots_y_start;
    if (!wide && y < lots_end) mvhline(y++, 1, ACS_HLINE, width - 2);

    const int xo = lot_x_offset;
    if (y < lots_end) label(y++, 2 + xo, "Open lots (per strategy)");
    if (y < lots_end)
    {
        attron(A_DIM);
        mvprintw(y, 2 + xo,  "%-8s", "Side");
        mvprintw(y, 10 + xo, "%-12s", "Symbol");
        mvprintw(y, 22 + xo, "%-16s", "Strategy");
        mvprintw(y, 38 + xo, "%14s", "Qty open");
        mvprintw(y, 54 + xo, "%14s", "Entry");
        mvprintw(y, 68 + xo, "%10s", "Age");
        attroff(A_DIM);
        ++y;
    }

    if (snap->lots.empty() && y < lots_end)
    {
        attron(A_DIM);
        mvaddstr(y++, 4 + xo, "(no open lots)");
        attroff(A_DIM);
    }
    else
    {
        std::size_t shown = 0;
        for (const auto& l : snap->lots)
        {
            if (y >= lots_end - 1 && shown < snap->lots.size())
            {
                if (snap->lots.size() - shown > 0)
                {
                    attron(A_DIM);
                    mvprintw(y, 4 + xo, "+ %zu more …",
                             snap->lots.size() - shown);
                    attroff(A_DIM);
                }
                break;
            }
            if (y >= lots_end) break;
            int spair = (l.side == 'L') ? kPairGreen
                       : (l.side == 'S') ? kPairRed : kPairWhite;
            attron(COLOR_PAIR(spair) | A_BOLD);
            mvaddstr(y, 2 + xo, l.side == 'L' ? "LONG"
                              : l.side == 'S' ? "SHORT" : "?");
            attroff(COLOR_PAIR(spair) | A_BOLD);
            mvprintw(y, 10 + xo, "%-12.12s", l.symbol.c_str());
            mvprintw(y, 22 + xo, "%-16.16s",
                     l.strategy_name.empty() ? "-" : l.strategy_name.c_str());
            mvprintw(y, 38 + xo, "%14.6f", l.qty_open);
            mvprintw(y, 54 + xo, "%14.4f", l.entry_price);
            mvprintw(y, 68 + xo, "%10s", fmt_age(l.age_seconds).c_str());
            ++y; ++shown;
        }
    }

    // Vertical divider in wide mode.
    if (wide)
    {
        attron(A_DIM);
        mvvline(body_y0, 78, ACS_VLINE, height);
        attroff(A_DIM);
    }

    // ── Session summary footer ─────────────────────────────────────────
    // One-line digest pinned to the last available row: round-trips,
    // win rate, and aggregate fills/orders. Hidden if there's no room.
    if (y < lots_end)
    {
        attron(A_DIM);
        char b[160];
        const std::size_t trades = snap->perf.total_trades;
        const std::size_t fills  = snap->perf.total_fills;
        const std::size_t orders = snap->perf.total_orders;
        const double      wr     = snap->perf.win_rate;
        std::snprintf(b, sizeof(b),
            "session: %zu trades  %zu fills  %zu orders  wr %.0f%%  sharpe %+.2f",
            trades, fills, orders, wr, snap->perf.sharpe);
        mvaddstr(lots_end - 1, 2, b);
        attroff(A_DIM);
    }
}

}

#endif // HAS_RICH_TUI
