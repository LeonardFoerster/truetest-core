#ifdef HAS_RICH_TUI

#include "orders_panel.h"

#include "../console_dashboard.h"
#include "../dashboard_snapshot.h"
#include "../tui_style.h"

#include <ncurses.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace truetest::ui {

namespace {

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

}

void OrdersPanel::draw(int body_y0, int width, int height,
                       const ConsoleDashboard& /*data*/,
                       const dashboard_snapshot* snap)
{
    if (!snap)
    {
        mvaddstr(body_y0, 2, "(no snapshot yet - engine warming up)");
        return;
    }

    int y = body_y0;
    int y_end = body_y0 + height;

    // Wide-mode (≥160 cols): open orders left, recent fills right at
    // full vertical height each. Narrow stays vertically split.
    const bool wide = (width >= 160);
    const int  fill_x_offset = wide ? 90 : 0;

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

    // Reserve the bottom half of the panel for fills, regardless of how
    // many open-order rows we have to render. In wide mode, both
    // sections use the full vertical height (side-by-side).
    const int orders_section_end = wide ? y_end : (body_y0 + height / 2);

    if (snap->open_orders.empty())
    {
        attron(A_DIM);
        mvaddstr(y++, 4, "(no working orders)");
        attroff(A_DIM);
    }
    else
    {
        std::vector<const dashboard_snapshot::open_order_row*> visible;
        visible.reserve(snap->open_orders.size());

        for (const auto& o : snap->open_orders)
        {
            if (filter_.empty() ||
                o.symbol.find(filter_) != std::string::npos ||
                std::string(o.status).find(filter_) != std::string::npos)
            {
                visible.push_back(&o);
            }
        }

        cursor_max_ = static_cast<int>(visible.size());
        if (cursor_row_ >= cursor_max_ && cursor_max_ > 0)
            cursor_row_ = cursor_max_ - 1;
        if (cursor_row_ < 0)
            cursor_row_ = 0;

        std::size_t shown = 0;
        for (std::size_t i = 0; i < visible.size(); ++i)
        {
            const auto& o = *visible[i];

            if (y >= orders_section_end - 1 && shown < visible.size())
            {
                if (visible.size() - shown > 0)
                {
                    attron(A_DIM);
                    mvprintw(y, 4, "+ %zu more …", visible.size() - shown);
                    attroff(A_DIM);
                }
                break;
            }
            if (y >= orders_section_end) break;

            const bool selected = (static_cast<int>(i) == cursor_row_);
            if (selected) attron(A_REVERSE);

            char idbuf[32];
            std::snprintf(idbuf, sizeof(idbuf), "%llu", static_cast<unsigned long long>(o.order_id));
            safe_mvaddstr(y, 2, 12, idbuf);
            int spair = (o.side == 'B') ? kPairGreen
                       : (o.side == 'S') ? kPairRed : kPairWhite;
            attron(COLOR_PAIR(spair) | A_BOLD);
            mvaddstr(y, 12, o.side == 'B' ? "BUY"  :
                            o.side == 'S' ? "SELL" : "?");
            attroff(COLOR_PAIR(spair) | A_BOLD);
            mvaddstr(y, 18, type_str(o.type));
            mvprintw(y, 23, "%-10.10s", o.symbol.c_str());
            mvprintw(y, 34, "%14.6f",   o.qty);
            if (o.type == 'M') mvprintw(y, 50, "%14s", "-");
            else               mvprintw(y, 50, "%14.4f", o.price);
            int statpair = (std::string(o.status) == "partial")
                              ? kPairYellow : kPairWhite;
            attron(COLOR_PAIR(statpair));
            mvprintw(y, 66, "%-9s", o.status);
            attroff(COLOR_PAIR(statpair));

            int agepair = (o.age_seconds >= 120) ? kPairRed
                        : (o.age_seconds >= 30)  ? kPairYellow
                        : kPairWhite;
            attron(COLOR_PAIR(agepair));
            mvprintw(y, 76, "%6s",  fmt_age(o.age_seconds).c_str());
            attroff(COLOR_PAIR(agepair));

            if (selected) attroff(A_REVERSE);

            ++y; ++shown;
        }
    }

    // Cursor hint
    if (cursor_max_ > 0 && y < orders_section_end)
    {
        attron(A_DIM);
        mvprintw(y, 4, "j/k: navigate (cursor on open orders)");
        attroff(A_DIM);
    }

    // Fills section. Narrow: stacked below; wide: right half, full height.
    const int fxo = fill_x_offset;
    if (wide)
    {
        y = body_y0;
        attron(A_DIM);
        mvvline(body_y0, 88, ACS_VLINE, height);
        attroff(A_DIM);
    }
    else
    {
        y = orders_section_end;
        if (y >= y_end) return;
        mvhline(y++, 1, ACS_HLINE, width - 2);
    }

    // ── Recent fills (with markout + side counts) ──
    label(y, 2 + fxo, "Recent fills");
    {
        // Compose: avg markout + buy/sell tally over the visible window.
        std::size_t buys = 0, sells = 0;
        double fee_sum = 0.0, qty_sum = 0.0;
        for (const auto& f : snap->recent_fills)
        {
            if (f.side == 'B') ++buys;
            else if (f.side == 'S') ++sells;
            fee_sum += f.fee;
            qty_sum += f.qty;
        }
        char b[160];
        if (snap->perf.markout_samples > 0)
            std::snprintf(b, sizeof(b),
                "  markout %+5.2fbp/n=%zu  buy %zu / sell %zu  Σfees %.4f  Σqty %.4f",
                snap->perf.avg_markout_bps, snap->perf.markout_samples,
                buys, sells, fee_sum, qty_sum);
        else
            std::snprintf(b, sizeof(b),
                "  buy %zu / sell %zu  Σfees %.4f  Σqty %.4f",
                buys, sells, fee_sum, qty_sum);
        attron(A_DIM);
        mvaddstr(y, 16 + fxo, b);
        attroff(A_DIM);
    }
    ++y;
    if (y >= y_end) return;

    attron(A_DIM);
    mvprintw(y, 2  + fxo, "%-10s", "Time");
    mvprintw(y, 12 + fxo, "%-5s",  "Side");
    mvprintw(y, 18 + fxo, "%-10s", "Symbol");
    mvprintw(y, 29 + fxo, "%14s",  "Qty");
    mvprintw(y, 45 + fxo, "%14s",  "Price");
    mvprintw(y, 61 + fxo, "%10s",  "Fee");
    mvprintw(y, 72 + fxo, "%-10s", "Source");
    attroff(A_DIM);
    ++y;

    if (snap->recent_fills.empty())
    {
        attron(A_DIM);
        mvaddstr(y++, 4 + fxo, "(no fills yet)");
        attroff(A_DIM);
    }
    else
    {
        std::size_t shown = 0;
        for (const auto& f : snap->recent_fills)
        {
            if (y >= y_end - 1 && shown < snap->recent_fills.size())
            {
                if (snap->recent_fills.size() - shown > 0)
                {
                    attron(A_DIM);
                    mvprintw(y, 4 + fxo, "+ %zu more …",
                             snap->recent_fills.size() - shown);
                    attroff(A_DIM);
                }
                break;
            }
            if (y >= y_end) break;
            mvprintw(y, 2 + fxo, "%-10s", fmt_hhmmss(f.ts).c_str());
            int spair = (f.side == 'B') ? kPairGreen
                       : (f.side == 'S') ? kPairRed : kPairWhite;
            attron(COLOR_PAIR(spair) | A_BOLD);
            mvaddstr(y, 12 + fxo, f.side == 'B' ? "BUY"  :
                                  f.side == 'S' ? "SELL" : "?");
            attroff(COLOR_PAIR(spair) | A_BOLD);
            mvprintw(y, 18 + fxo, "%-10.10s", f.symbol.c_str());
            mvprintw(y, 29 + fxo, "%14.6f",   f.qty);
            mvprintw(y, 45 + fxo, "%14.4f",   f.price);
            // Fee dim if zero (no fee model loaded), red if non-trivial.
            int fee_pair = (f.fee > 0.0) ? kPairRed : kPairWhite;
            attron(COLOR_PAIR(fee_pair));
            mvprintw(y, 61 + fxo, "%10.4f",   f.fee);
            attroff(COLOR_PAIR(fee_pair));
            // Source: cyan for "exchange" (real fill), white otherwise.
            int src_pair = (std::string(f.source) == "exchange") ? kPairCyan
                         : kPairWhite;
            attron(COLOR_PAIR(src_pair));
            mvprintw(y, 72 + fxo, "%-10s",    f.source);
            attroff(COLOR_PAIR(src_pair));
            ++y; ++shown;
        }
    }
}

}

#endif // HAS_RICH_TUI
