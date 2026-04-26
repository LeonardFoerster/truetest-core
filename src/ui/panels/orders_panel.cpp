#ifdef HAS_RICH_TUI

#include "orders_panel.h"

#include "../console_dashboard.h"
#include "../dashboard_snapshot.h"

#include <ncurses.h>

#include <algorithm>
#include <chrono>
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

const char* type_str(char t)
{
    switch (t)
    {
        case 'M': return "MKT";
        case 'L': return "LMT";
        case 'S': return "STP";
        case 's': return "SLM";
        default:  return "?";
    }
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

} // namespace

void OrdersPanel::draw(int body_y0, int width, int height,
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

    // ── Open orders ──
    label(y, 2, "Open orders");
    {
        char b[24];
        std::snprintf(b, sizeof(b), " (%zu)", snap->open_orders.size());
        attron(A_DIM);
        mvaddstr(y, 14, b);
        attroff(A_DIM);
    }
    ++y;
    if (y >= y_end) return;

    attron(A_DIM);
    mvprintw(y, 2,  "%-10s", "Order ID");
    mvprintw(y, 12, "%-5s",  "Side");
    mvprintw(y, 18, "%-4s",  "Typ");
    mvprintw(y, 23, "%-10s", "Symbol");
    mvprintw(y, 34, "%14s",  "Qty");
    mvprintw(y, 50, "%14s",  "Price");
    mvprintw(y, 66, "%-9s",  "Status");
    mvprintw(y, 76, "%6s",   "Age");
    attroff(A_DIM);
    ++y;

    if (snap->open_orders.empty())
    {
        attron(A_DIM);
        mvaddstr(y++, 4, "(no working orders)");
        attroff(A_DIM);
    }
    else
    {
        // Print up to N rows; older rows get clipped if the window is small.
        int max_rows = (y_end - y) / 2;          // leave space for fills below
        if (max_rows < 1) max_rows = 1;
        int shown = 0;
        for (const auto& o : snap->open_orders)
        {
            if (shown >= max_rows || y >= y_end) break;
            mvprintw(y, 2,  "%-10llu",
                     static_cast<unsigned long long>(o.order_id));
            int spair = (o.side == 'B') ? kPairGreen
                       : (o.side == 'S') ? kPairRed : kPairWhite;
            attron(COLOR_PAIR(spair) | A_BOLD);
            mvaddstr(y, 12, o.side == 'B' ? "BUY"  :
                            o.side == 'S' ? "SELL" : "?");
            attroff(COLOR_PAIR(spair) | A_BOLD);
            mvaddstr(y, 18, type_str(o.type));
            mvprintw(y, 23, "%-10.10s", o.symbol.c_str());
            mvprintw(y, 34, "%14.6f",   o.qty);
            if (o.type == 'M') mvprintw(y, 50, "%14s", "—");
            else               mvprintw(y, 50, "%14.4f", o.price);
            int statpair = (std::string(o.status) == "partial")
                              ? kPairYellow : kPairWhite;
            attron(COLOR_PAIR(statpair));
            mvprintw(y, 66, "%-9s", o.status);
            attroff(COLOR_PAIR(statpair));
            mvprintw(y, 76, "%6s",  fmt_age(o.age_seconds).c_str());
            ++y; ++shown;
        }
    }

    if (y >= y_end - 2) return;
    mvhline(y++, 1, ACS_HLINE, width - 2);

    // ── Recent fills (with markout summary) ──
    label(y, 2, "Recent fills");
    {
        char b[64];
        if (snap->perf.markout_samples > 0)
            std::snprintf(b, sizeof(b),
                          "  avg markout: %+5.2f bps over %zu fills",
                          snap->perf.avg_markout_bps,
                          snap->perf.markout_samples);
        else
            std::snprintf(b, sizeof(b), "  avg markout: —");
        attron(A_DIM);
        mvaddstr(y, 16, b);
        attroff(A_DIM);
    }
    ++y;
    if (y >= y_end) return;

    attron(A_DIM);
    mvprintw(y, 2,  "%-10s", "Time");
    mvprintw(y, 12, "%-5s",  "Side");
    mvprintw(y, 18, "%-10s", "Symbol");
    mvprintw(y, 29, "%14s",  "Qty");
    mvprintw(y, 45, "%14s",  "Price");
    mvprintw(y, 61, "%10s",  "Fee");
    mvprintw(y, 72, "%-10s", "Source");
    attroff(A_DIM);
    ++y;

    if (snap->recent_fills.empty())
    {
        attron(A_DIM);
        mvaddstr(y++, 4, "(no fills yet)");
        attroff(A_DIM);
    }
    else
    {
        for (const auto& f : snap->recent_fills)
        {
            if (y >= y_end) break;
            mvprintw(y, 2, "%-10s", fmt_hhmmss(f.ts).c_str());
            int spair = (f.side == 'B') ? kPairGreen
                       : (f.side == 'S') ? kPairRed : kPairWhite;
            attron(COLOR_PAIR(spair) | A_BOLD);
            mvaddstr(y, 12, f.side == 'B' ? "BUY"  :
                            f.side == 'S' ? "SELL" : "?");
            attroff(COLOR_PAIR(spair) | A_BOLD);
            mvprintw(y, 18, "%-10.10s", f.symbol.c_str());
            mvprintw(y, 29, "%14.6f",   f.qty);
            mvprintw(y, 45, "%14.4f",   f.price);
            mvprintw(y, 61, "%10.4f",   f.fee);
            mvprintw(y, 72, "%-10s",    f.source);
            ++y;
        }
    }
}

} // namespace truetest::ui

#endif // HAS_RICH_TUI
