#ifdef HAS_RICH_TUI

#include "l2_panel.h"

#include "../console_dashboard.h"
#include "../dashboard_snapshot.h"
#include "../tui_style.h"

#include <ncurses.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

const char* source_text(dashboard_snapshot::l2_source s)
{
    switch (s) {
        case dashboard_snapshot::l2_source::venue:     return " venue depth ";
        case dashboard_snapshot::l2_source::synthetic: return " synthetic (MM) ";
        case dashboard_snapshot::l2_source::none:      return " empty ";
    }
    return " ? ";
}

int source_pair(dashboard_snapshot::l2_source s)
{
    switch (s) {
        case dashboard_snapshot::l2_source::venue:     return kPairCyan;
        case dashboard_snapshot::l2_source::synthetic: return kPairYellow;
        default:                                       return kPairWhite;
    }
}

}

void L2Panel::draw(int body_y0, int width, int height,
                   const ConsoleDashboard& data,
                   const dashboard_snapshot* snap)
{
    (void)data;

    if (!snap)
    {
        mvaddstr(body_y0, 2, "(no snapshot yet — engine warming up)");
        return;
    }
    const auto& v = snap->l2;

    int y = body_y0;
    const int y_end = body_y0 + height;

    // ── Header: symbol + source badge ──
    label(y, 2, "L2 depth");
    {
        const std::string sym = v.symbol.empty() ? std::string("(no symbol)") : v.symbol;
        attron(A_BOLD);
        mvprintw(y, 12, "%-12.12s", sym.c_str());
        attroff(A_BOLD);

        // Source badge.
        const int sp = source_pair(v.source);
        attron(COLOR_PAIR(sp) | A_REVERSE | A_BOLD);
        mvaddstr(y, 26, source_text(v.source));
        attroff(COLOR_PAIR(sp) | A_REVERSE | A_BOLD);

        // Right-side info: total depth across both sides.
        if (v.source != dashboard_snapshot::l2_source::none)
        {
            char b[96];
            std::snprintf(b, sizeof(b),
                "%zu bid lvls · %zu ask lvls (showing top %zu)",
                v.total_bid_levels, v.total_ask_levels,
                std::max(v.bids.size(), v.asks.size()));
            attron(A_DIM);
            mvaddstr(y, 50, b);
            attroff(A_DIM);
        }
    }
    ++y;
    if (y >= y_end) return;

    // ── Empty state ──
    if (v.source == dashboard_snapshot::l2_source::none ||
        (v.bids.empty() && v.asks.empty()))
    {
        mvhline(y++, 1, ACS_HLINE, width - 2);
        if (y >= y_end) return;
        attron(A_DIM);
        mvaddstr(y++, 4, "(no depth available for this symbol)");
        if (y < y_end)
            mvaddstr(y++, 4,
                "Enable real venue depth: --provider binance --depth-stream depth20@100ms");
        attroff(A_DIM);
        return;
    }

    // Source caveat: synthetic depth is paper-only — make sure no one
    // mistakes it for the real venue book.
    if (v.source == dashboard_snapshot::l2_source::synthetic && y < y_end)
    {
        attron(A_DIM | COLOR_PAIR(kPairYellow));
        mvaddstr(y++, 2,
            "  ⚠ synthetic depth (MarketMaker-seeded). Not the real venue book.");
        attroff(A_DIM | COLOR_PAIR(kPairYellow));
    }

    if (y >= y_end) return;
    mvhline(y++, 1, ACS_HLINE, width - 2);
    if (y >= y_end) return;

    // ── Column header ──
    attron(A_DIM);
    mvprintw(y, 2,  "%-5s  %12s  %14s  %14s  %s",
             "side", "price", "size", "cumulative", "depth bar");
    attroff(A_DIM);
    ++y;
    if (y >= y_end) return;

    // Compute the max size on the page so all bars share the same scale.
    double max_size = 0.0;
    for (const auto& l : v.bids) max_size = std::max(max_size, l.size);
    for (const auto& l : v.asks) max_size = std::max(max_size, l.size);

    // Leave columns:  side(7) price(14) size(16) cum(16) bar(rest)
    const int bar_x = 2 + 7 + 14 + 16 + 16;
    const int bar_w = std::max(8, width - bar_x - 2);

    // Bars built into a single buffer + emitted via mvaddstr — one
    // ncurses call per bar instead of `filled` mvaddch calls. With
    // 20 levels × ~30-cell bars that's 600 → 20 calls per render.
    char bar_buf[256];
    auto draw_bar = [&](int yy, double size, int color_pair) {
        if (max_size <= 0.0) return;
        const int filled = std::min<int>({bar_w,
            static_cast<int>(std::lround(size / max_size * bar_w)),
            static_cast<int>(sizeof(bar_buf) - 1)});
        std::memset(bar_buf, '#', filled);
        bar_buf[filled] = '\0';
        attron(COLOR_PAIR(color_pair));
        mvaddstr(yy, bar_x, bar_buf);
        attroff(COLOR_PAIR(color_pair));
    };

    // Reserve vertical: ~half for asks (rendered top-down high → low),
    // mid-line, ~half for bids (rendered top-down high → low).
    const int rows_avail = (y_end - y) - 4;        // leave 4 for mid + footer
    const int per_side   = std::max(1, rows_avail / 2);
    const int n_asks     = std::min<int>(per_side, static_cast<int>(v.asks.size()));
    const int n_bids     = std::min<int>(per_side, static_cast<int>(v.bids.size()));

    // ── Asks: render reversed (worst → best) so the best ask sits
    // immediately above the mid-line, matching trader intuition.
    for (int i = n_asks - 1; i >= 0; --i)
    {
        if (y >= y_end) break;
        const auto& l = v.asks[i];
        attron(COLOR_PAIR(kPairRed));
        mvaddstr(y, 2, "ASK");
        attroff(COLOR_PAIR(kPairRed));
        mvprintw(y, 9,  "%14.4f", l.price);
        mvprintw(y, 25, "%14.6f", l.size);
        attron(A_DIM);
        mvprintw(y, 41, "%14.6f", l.cum);
        attroff(A_DIM);
        draw_bar(y, l.size, kPairRed);
        ++y;
    }

    // ── Mid-line: spread + microprice + best bid/ask ──
    if (y < y_end)
    {
        attron(COLOR_PAIR(kPairCyan) | A_BOLD);
        mvhline(y, 1, ACS_HLINE, width - 2);
        char b[160];
        std::snprintf(b, sizeof(b),
            " mid %.4f · spread %.2f bp · microprice %.4f ",
            v.mid, v.spread_bps, v.microprice);
        const int x = std::max(2, (width - static_cast<int>(std::strlen(b))) / 2);
        mvaddstr(y, x, b);
        attroff(COLOR_PAIR(kPairCyan) | A_BOLD);
        ++y;
    }
    if (y >= y_end) return;

    // ── Bids: render top-down (best at top, worst at bottom) ──
    for (int i = 0; i < n_bids; ++i)
    {
        if (y >= y_end) break;
        const auto& l = v.bids[i];
        attron(COLOR_PAIR(kPairGreen));
        mvaddstr(y, 2, "BID");
        attroff(COLOR_PAIR(kPairGreen));
        mvprintw(y, 9,  "%14.4f", l.price);
        mvprintw(y, 25, "%14.6f", l.size);
        attron(A_DIM);
        mvprintw(y, 41, "%14.6f", l.cum);
        attroff(A_DIM);
        draw_bar(y, l.size, kPairGreen);
        ++y;
    }

    // ── Footer: imbalance bar + cumulative totals ──
    if (y < y_end - 1)
    {
        ++y;
        if (y < y_end) mvhline(y++, 1, ACS_HLINE, width - 2);
        if (y < y_end)
        {
            // Imbalance bar centered on neutral. Range [-1, +1].
            const int half = std::max(8, std::min(20, (width - 40) / 2));
            const int center_x = 2 + 12 + half;   // after label
            label(y, 2, "imbalance");
            // Frame: [   ░░░░ | ░░░░   ]  with a center notch.
            mvaddch(y, center_x - half, '[');
            for (int i = 1; i < 2 * half; ++i)
                mvaddch(y, center_x - half + i, ' ');
            mvaddch(y, center_x + half, ']');
            mvaddch(y, center_x, ACS_VLINE);
            // Bar segment off the center, width proportional to |imb|.
            const double imb = std::clamp(v.imbalance, -1.0, 1.0);
            const int segw = static_cast<int>(std::lround(std::fabs(imb) * (half - 1)));
            const int color = (imb >= 0) ? kPairGreen : kPairRed;
            attron(COLOR_PAIR(color));
            if (imb >= 0)
                for (int i = 0; i < segw; ++i)
                    mvaddch(y, center_x + 1 + i, ACS_CKBOARD);
            else
                for (int i = 0; i < segw; ++i)
                    mvaddch(y, center_x - 1 - i, ACS_CKBOARD);
            attroff(COLOR_PAIR(color));

            char b[64];
            std::snprintf(b, sizeof(b), " %+5.1f%%   bids %.4f / asks %.4f",
                          imb * 100.0, v.cum_bid_size, v.cum_ask_size);
            mvaddstr(y, center_x + half + 2, b);
            ++y;
        }
    }
}

}

#endif // HAS_RICH_TUI
