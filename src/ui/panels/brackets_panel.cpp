#ifdef HAS_RICH_TUI

#include "brackets_panel.h"

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

std::string fmt_age(std::int64_t s)
{
    char b[16];
    if (s < 60)        std::snprintf(b, sizeof(b), "%llds", (long long)s);
    else if (s < 3600) std::snprintf(b, sizeof(b), "%lldm%llds", (long long)(s/60), (long long)(s%60));
    else               std::snprintf(b, sizeof(b), "%lldh%lldm",  (long long)(s/3600), (long long)((s%3600)/60));
    return b;
}

// Distance from mark to a trigger price as % of mark, signed so negative
// means the bracket is *behind* the current price (already crossed).
double dist_pct(double mark, double trigger)
{
    if (mark <= 0.0 || trigger <= 0.0) return 0.0;
    return (trigger - mark) / mark * 100.0;
}

// Color tier for distance-to-trigger. Magnitude-based: closer = hotter.
int dist_pair(double abs_pct)
{
    if (abs_pct <= 0.10) return kPairRed;
    if (abs_pct <= 0.50) return kPairYellow;
    return kPairWhite;
}

} // namespace

void BracketsPanel::draw(int body_y0, int width, int height,
                         const ConsoleDashboard& data,
                         const dashboard_snapshot* snap)
{
    (void)data;
    (void)width;

    if (!snap)
    {
        mvaddstr(body_y0, 2, "(no snapshot yet — engine warming up)");
        return;
    }

    int y = body_y0;
    const int y_end = body_y0 + height;

    label(y, 2, "Active brackets");
    {
        char b[64];
        std::size_t venue = 0;
        for (const auto& r : snap->brackets) if (r.venue_managed) ++venue;
        std::snprintf(b, sizeof(b),
            " (%zu armed · %zu venue-resting · %zu engine-only)",
            snap->brackets.size(), venue, snap->brackets.size() - venue);
        attron(A_DIM);
        mvaddstr(y, 19, b);
        attroff(A_DIM);
    }
    ++y;
    if (y >= y_end) return;

    // Column layout. Wide enough that "venue list-id" stays visible
    // when present, but degrades gracefully on narrow terminals.
    attron(A_DIM);
    mvprintw(y, 2,  "%-7s",  "OpId");
    mvprintw(y, 10, "%-3s",  "Sd");
    mvprintw(y, 14, "%-9s",  "Symbol");
    mvprintw(y, 24, "%10s",  "Qty");
    mvprintw(y, 35, "%10s",  "Entry");
    mvprintw(y, 46, "%10s",  "Mark");
    mvprintw(y, 57, "%10s",  "SL");
    mvprintw(y, 68, "%9s",   "→SL%");
    mvprintw(y, 78, "%10s",  "TP");
    mvprintw(y, 89, "%9s",   "→TP%");
    mvprintw(y, 99, "%-7s",  "Where");
    mvprintw(y, 107, "%6s",  "Age");
    mvprintw(y, 114, "%-12s", "Strategy");
    attroff(A_DIM);
    ++y;

    // Apply filter (substring match against symbol or strategy name).
    std::vector<const dashboard_snapshot::bracket_row*> visible;
    visible.reserve(snap->brackets.size());
    for (const auto& r : snap->brackets)
    {
        if (filter_.empty()
            || r.symbol.find(filter_)        != std::string::npos
            || r.strategy_name.find(filter_) != std::string::npos)
            visible.push_back(&r);
    }
    cursor_max_ = static_cast<int>(visible.size());
    if (cursor_ >= cursor_max_) cursor_ = std::max(0, cursor_max_ - 1);

    if (visible.empty())
    {
        attron(A_DIM);
        if (filter_.empty())
            mvaddstr(y++, 4, "(no armed brackets — strategies haven't entered yet)");
        else
            mvaddstr(y++, 4,
                ("(no brackets match filter '" + filter_ + "')").c_str());
        attroff(A_DIM);
    }
    else
    {
        int row_idx = 0;
        for (const auto* rp : visible)
        {
            const auto& r = *rp;
            if (y >= y_end) break;
            const bool selected = (row_idx == cursor_);
            if (selected) attron(A_REVERSE);
            ++row_idx;

            mvprintw(y, 2,  "%-7llu", (unsigned long long)r.opener_order_id);

            int spair = (r.side == 'L') ? kPairGreen
                       : (r.side == 'S') ? kPairRed : kPairWhite;
            attron(COLOR_PAIR(spair) | A_BOLD);
            mvaddstr(y, 10, r.side == 'L' ? "L"
                          : r.side == 'S' ? "S" : "?");
            attroff(COLOR_PAIR(spair) | A_BOLD);

            mvprintw(y, 14, "%-9.9s", r.symbol.c_str());
            mvprintw(y, 24, "%10.6f", r.qty);
            mvprintw(y, 35, "%10.4f", r.entry_price);

            if (r.mark > 0.0) mvprintw(y, 46, "%10.4f", r.mark);
            else              mvprintw(y, 46, "%10s", "—");

            // SL price + distance.
            if (r.stop_loss)
            {
                mvprintw(y, 57, "%10.4f", *r.stop_loss);
                if (r.mark > 0.0)
                {
                    const double d = dist_pct(r.mark, *r.stop_loss);
                    int p = dist_pair(std::abs(d));
                    attron(COLOR_PAIR(p));
                    mvprintw(y, 68, "%+8.2f%%", d);
                    attroff(COLOR_PAIR(p));
                }
                else mvprintw(y, 68, "%9s", "—");
            }
            else
            {
                attron(A_DIM);
                mvprintw(y, 57, "%10s", "—");
                mvprintw(y, 68, "%9s",  "—");
                attroff(A_DIM);
            }

            // TP price + distance.
            if (r.take_profit)
            {
                mvprintw(y, 78, "%10.4f", *r.take_profit);
                if (r.mark > 0.0)
                {
                    const double d = dist_pct(r.mark, *r.take_profit);
                    int p = dist_pair(std::abs(d));
                    attron(COLOR_PAIR(p));
                    mvprintw(y, 89, "%+8.2f%%", d);
                    attroff(COLOR_PAIR(p));
                }
                else mvprintw(y, 89, "%9s", "—");
            }
            else
            {
                attron(A_DIM);
                mvprintw(y, 78, "%10s", "—");
                mvprintw(y, 89, "%9s",  "—");
                attroff(A_DIM);
            }

            // Where: venue-resting (cyan) vs engine-only watchdog (yellow).
            int wpair = r.venue_managed ? kPairCyan : kPairYellow;
            attron(COLOR_PAIR(wpair));
            mvprintw(y, 99, "%-7s", r.venue_managed ? "venue" : "engine");
            attroff(COLOR_PAIR(wpair));

            mvprintw(y, 107, "%6s", fmt_age(r.age_seconds).c_str());
            mvprintw(y, 114, "%-12.12s",
                     r.strategy_name.empty() ? "—" : r.strategy_name.c_str());

            if (selected) attroff(A_REVERSE);
            ++y;
        }

        // Drill-in: render full detail of the selected bracket below
        // the table.
        if (cursor_ < static_cast<int>(visible.size()) && y < y_end - 1)
        {
            ++y;
            const auto& r = *visible[cursor_];
            attron(COLOR_PAIR(kPairCyan) | A_BOLD);
            mvprintw(y++, 2, "── Detail: opener %llu ──",
                     (unsigned long long)r.opener_order_id);
            attroff(COLOR_PAIR(kPairCyan) | A_BOLD);
            if (y < y_end)
                mvprintw(y++, 4, "symbol=%s  side=%c  qty=%.6f  entry=%.4f",
                         r.symbol.c_str(), r.side, r.qty, r.entry_price);
            if (y < y_end)
                mvprintw(y++, 4,
                    "stop_loss=%s  take_profit=%s  mark=%.4f",
                    r.stop_loss   ? std::to_string(*r.stop_loss).c_str()  : "—",
                    r.take_profit ? std::to_string(*r.take_profit).c_str(): "—",
                    r.mark);
            if (y < y_end)
                mvprintw(y++, 4,
                    "venue_managed=%s  list_id=%s  age=%llds",
                    r.venue_managed ? "yes" : "no",
                    r.venue_list_id.empty() ? "—" : r.venue_list_id.c_str(),
                    (long long)r.age_seconds);
        }
    }

    // ── Footer: venue list-ids for the resting brackets ──
    if (y < y_end - 1)
    {
        bool any = false;
        for (const auto& r : snap->brackets)
            if (r.venue_managed && !r.venue_list_id.empty()) { any = true; break; }
        if (any)
        {
            ++y;
            label(y++, 2, "Venue list-ids");
            for (const auto& r : snap->brackets)
            {
                if (y >= y_end) break;
                if (!r.venue_managed || r.venue_list_id.empty()) continue;
                attron(A_DIM);
                mvprintw(y, 4, "opener=%llu  list=%s",
                         (unsigned long long)r.opener_order_id,
                         r.venue_list_id.c_str());
                attroff(A_DIM);
                ++y;
            }
        }
    }
}

} // namespace truetest::ui

#endif // HAS_RICH_TUI
