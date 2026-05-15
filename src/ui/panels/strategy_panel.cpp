#ifdef HAS_RICH_TUI

#include "strategy_panel.h"

#include "../console_dashboard.h"
#include "../dashboard_snapshot.h"

#include <ncurses.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

}

void StrategyPanel::draw(int body_y0, int width, int height,
                         const ConsoleDashboard& data,
                         const dashboard_snapshot* snap)
{
    (void)data;

    if (!snap)
    {
        mvaddstr(body_y0, 2, "(no snapshot yet — engine warming up)");
        return;
    }

    int y = body_y0;
    const int y_end = body_y0 + height;

    label(y, 2, "Per-strategy attribution");
    {
        char b[64];
        std::snprintf(b, sizeof(b), " (%zu active)", snap->strategies.size());
        attron(A_DIM);
        mvaddstr(y, 27, b);
        attroff(A_DIM);
    }
    ++y;
    if (y >= y_end) return;

    attron(A_DIM);
    mvprintw(y, 2,   "%-18s", "Strategy");
    mvprintw(y, 21,  "%12s",  "PnL");
    mvprintw(y, 34,  "%8s",   "Trades");
    mvprintw(y, 43,  "%8s",   "Wins");
    mvprintw(y, 52,  "%8s",   "WinRate");
    mvprintw(y, 61,  "%8s",   "PF");
    mvprintw(y, 70,  "%12s",  "ΣWin");
    mvprintw(y, 83,  "%12s",  "ΣLoss");
    mvprintw(y, 96,  "%6s",   "Lots");
    mvprintw(y, 103, "%8s",   "Brkts");
    attroff(A_DIM);
    ++y;

    if (snap->strategies.empty())
    {
        attron(A_DIM);
        mvaddstr(y++, 4, "(no per-strategy attribution yet — first fill establishes a row)");
        attroff(A_DIM);
    }
    else
    {
        // Sort by absolute PnL descending so the biggest movers float
        // to the top — easier to spot regressions in a multi-strategy run.
        auto rows = snap->strategies;
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return std::abs(a.pnl) > std::abs(b.pnl);
        });

        for (const auto& s : rows)
        {
            if (y >= y_end) break;

            mvprintw(y, 2, "%-18.18s",
                     s.name.empty() ? "—" : s.name.c_str());

            // PnL — color-graded by sign + magnitude.
            int p = signed_pair(s.pnl);
            attron(COLOR_PAIR(p) | (std::abs(s.pnl) > 100.0 ? A_BOLD : 0));
            mvprintw(y, 21, "%+12.4f", s.pnl);
            attroff(COLOR_PAIR(p) | (std::abs(s.pnl) > 100.0 ? A_BOLD : 0));

            mvprintw(y, 34, "%8zu", s.trade_count);
            mvprintw(y, 43, "%8zu", s.win_count);

            // WinRate — green ≥55, white ≥45, yellow below.
            int wpair = (s.win_rate >= 55.0) ? kPairGreen
                      : (s.win_rate >= 45.0) ? kPairWhite : kPairYellow;
            attron(COLOR_PAIR(wpair));
            mvprintw(y, 52, "%7.1f%%", s.win_rate);
            attroff(COLOR_PAIR(wpair));

            // Profit factor — green > 1.5, white 1.0–1.5, red < 1.
            int pfpair = (s.profit_factor >= 1.5) ? kPairGreen
                       : (s.profit_factor >= 1.0) ? kPairWhite : kPairRed;
            attron(COLOR_PAIR(pfpair));
            if (s.profit_factor >= 1e8)
                mvprintw(y, 61, "%8s", "∞");
            else
                mvprintw(y, 61, "%8.2f", s.profit_factor);
            attroff(COLOR_PAIR(pfpair));

            attron(COLOR_PAIR(kPairGreen));
            mvprintw(y, 70, "%12.4f", s.total_win);
            attroff(COLOR_PAIR(kPairGreen));
            attron(COLOR_PAIR(kPairRed));
            mvprintw(y, 83, "%12.4f", s.total_loss);
            attroff(COLOR_PAIR(kPairRed));

            // Lots/Brackets — bold cyan when active; dim when zero.
            if (s.open_lots > 0) {
                attron(COLOR_PAIR(kPairCyan) | A_BOLD);
                mvprintw(y, 96, "%6zu", s.open_lots);
                attroff(COLOR_PAIR(kPairCyan) | A_BOLD);
            } else {
                attron(A_DIM);
                mvprintw(y, 96, "%6zu", s.open_lots);
                attroff(A_DIM);
            }
            if (s.armed_brackets > 0) {
                attron(COLOR_PAIR(kPairCyan) | A_BOLD);
                mvprintw(y, 103, "%8zu", s.armed_brackets);
                attroff(COLOR_PAIR(kPairCyan) | A_BOLD);
            } else {
                attron(A_DIM);
                mvprintw(y, 103, "%8zu", s.armed_brackets);
                attroff(A_DIM);
            }

            ++y;
        }
    }

    // ── Aggregate footer ──
    if (y < y_end - 1)
    {
        ++y;
        if (y < y_end) mvhline(y++, 1, ACS_HLINE, width - 2);
        if (y < y_end)
        {
            double tot_pnl = 0.0, tot_win = 0.0, tot_loss = 0.0;
            std::size_t tot_trades = 0, tot_wins = 0;
            std::size_t tot_lots = 0, tot_brkts = 0;
            for (const auto& s : snap->strategies)
            {
                tot_pnl    += s.pnl;
                tot_win    += s.total_win;
                tot_loss   += s.total_loss;
                tot_trades += s.trade_count;
                tot_wins   += s.win_count;
                tot_lots   += s.open_lots;
                tot_brkts  += s.armed_brackets;
            }
            const double agg_wr = tot_trades > 0
                ? static_cast<double>(tot_wins) / tot_trades * 100.0 : 0.0;
            const double agg_pf = tot_loss > 0.0 ? tot_win / tot_loss : 0.0;
            char b[256];
            std::snprintf(b, sizeof(b),
                "  Σ: %+10.4f pnl  %zu trades  %.1f%% wr  pf %.2f  lots %zu  brkts %zu",
                tot_pnl, tot_trades, agg_wr, agg_pf, tot_lots, tot_brkts);
            int p = signed_pair(tot_pnl);
            attron(COLOR_PAIR(p) | A_BOLD);
            mvaddstr(y, 2, b);
            attroff(COLOR_PAIR(p) | A_BOLD);
        }
    }
}

}

#endif // HAS_RICH_TUI
