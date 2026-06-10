#ifdef HAS_RICH_TUI

#include "debug_panel.h"

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

void section(int y, int x, int width, const char* text)
{
    attron(COLOR_PAIR(kPairCyan) | A_BOLD);
    mvprintw(y, x, "─── %s ", text);
    attroff(COLOR_PAIR(kPairCyan) | A_BOLD);
    int used = static_cast<int>(std::strlen(text)) + 5;
    attron(A_DIM);
    for (int i = x + used; i < width - 1; ++i) mvaddch(y, i, ACS_HLINE);
    attroff(A_DIM);
}

int yes_no_pair(bool b) { return b ? kPairGreen : kPairWhite; }

}

void DebugPanel::draw(int body_y0, int width, int height,
                      const ConsoleDashboard& data,
                      const dashboard_snapshot* snap)
{
    (void)data;

    if (!snap)
    {
        mvaddstr(body_y0, 2, "(no snapshot yet — engine warming up)");
        return;
    }
    const auto& d = snap->debug;

    int y = body_y0;
    const int y_end = body_y0 + height;
    const int x_l_label = 2,  x_l_value = 22;
    const int x_r_label = 52, x_r_value = 72;

    // ── Build / Target ──
    section(y++, 0, width, "Build / Target");
    if (y >= y_end) return;
    label(y, x_l_label, "binary");
    mvprintw(y, x_l_value, "%-20.20s", d.target.c_str());
    label(y, x_r_label, "mode");
    mvprintw(y, x_r_value, "%-20.20s", d.mode.c_str());
    ++y;
    if (y >= y_end) return;
    auto put_def = [](int yy, int xl, int xv, const char* nm, bool on) {
        attron(A_DIM); mvaddstr(yy, xl, nm); attroff(A_DIM);
        attron(COLOR_PAIR(yes_no_pair(on)) | (on ? A_BOLD : 0));
        mvaddstr(yy, xv, on ? "yes" : " no");
        attroff(COLOR_PAIR(yes_no_pair(on)) | (on ? A_BOLD : 0));
    };
    put_def(y, x_l_label, x_l_value,        "HAS_BINANCE",   d.has_binance);
    put_def(y, x_l_label + 18, x_l_value + 16, "HAS_QUESTDB",  d.has_questdb);
    put_def(y, x_r_label, x_r_value,        "HAS_DEBUG",     d.has_debug);
    put_def(y, x_r_label + 16, x_r_value + 14, "HAS_LIVE_DATA", d.has_live_data);
    ++y;

    // ── Threading ──
    if (y < y_end - 1) section(y++, 0, width, "Threading");
    if (y >= y_end) return;
    label(y, x_l_label, "preset");
    mvprintw(y, x_l_value, "%-12.12s", d.preset.c_str());
    label(y, x_l_label + 22, "workers");
    mvprintw(y, x_l_value + 22, "%zu", d.worker_count);
    label(y, x_r_label, "cpu-pin");
    attron(COLOR_PAIR(yes_no_pair(d.cpu_pin)));
    mvaddstr(y, x_r_value, d.cpu_pin ? "yes" : " no");
    attroff(COLOR_PAIR(yes_no_pair(d.cpu_pin)));
    label(y, x_r_label + 14, "spin");
    mvprintw(y, x_r_value + 12, "%-10.10s", d.spin_policy.c_str());
    ++y;

    // ── Rings ──
    if (y < y_end - 1) section(y++, 0, width, "Rings  (cur / hwm / cap, drops)");
    if (y >= y_end) return;
    attron(A_DIM);
    mvprintw(y, x_l_label, "%-12s %10s %10s %10s %10s",
             "name", "cur", "hwm", "cap", "drops");
    attroff(A_DIM);
    ++y;
    for (const auto& r : d.rings)
    {
        if (y >= y_end) break;
        if (r.capacity == 0)
        {
            attron(A_DIM);
            mvprintw(y, x_l_label, "%-12s %10s %10s %10s %10s",
                     r.name, "—", "—", "—", "—");
            attroff(A_DIM);
        }
        else
        {
            mvprintw(y, x_l_label, "%-12s %10zu", r.name, r.size);
            // HWM color: yellow > 50% capacity, red > 90%.
            const double frac = static_cast<double>(r.hwm) / r.capacity;
            int hwm_pair = (frac > 0.9) ? kPairRed
                         : (frac > 0.5) ? kPairYellow : kPairWhite;
            attron(COLOR_PAIR(hwm_pair));
            mvprintw(y, x_l_label + 23, "%10zu", r.hwm);
            attroff(COLOR_PAIR(hwm_pair));
            mvprintw(y, x_l_label + 34, "%10zu", r.capacity);
            int dp = (r.drops > 0) ? kPairRed : kPairWhite;
            attron(COLOR_PAIR(dp));
            mvprintw(y, x_l_label + 45, "%10llu",
                     (unsigned long long)r.drops);
            attroff(COLOR_PAIR(dp));
        }
        ++y;
    }

    // ── Object pools ──
    if (y < y_end - 1) section(y++, 0, width, "Object pools  (blocks × block-size = capacity)");
    if (y >= y_end) return;
    attron(A_DIM);
    mvprintw(y, x_l_label, "%-14s %6s %6s %8s %6s %5s",
             "name", "blocks", "in-use", "capacity", "grow", "fill%");
    attroff(A_DIM);
    ++y;
    for (const auto& p : d.pools)
    {
        if (y >= y_end) break;
        const unsigned fill_pct = (p.capacity > 0)
            ? static_cast<unsigned>((p.in_use * 100) / p.capacity)
            : 0u;
        const int grow_pair = (p.grow_count > 0) ? kPairRed : kPairWhite;
        mvprintw(y, x_l_label, "%-14s %6zu %6zu %8zu",
                 p.name, p.blocks, p.in_use, p.capacity);
        attron(COLOR_PAIR(grow_pair));
        mvprintw(y, x_l_label + 38, "%6zu", p.grow_count);
        attroff(COLOR_PAIR(grow_pair));
        mvprintw(y, x_l_label + 46, "%5u%%", fill_pct);
        ++y;
    }

    // ── Engine state ──
    if (y < y_end - 1) section(y++, 0, width, "Engine state");
    if (y >= y_end) return;
    auto kv = [&](int yy, int xl, int xv, const char* k, std::size_t v) {
        label(yy, xl, k);
        mvprintw(yy, xv, "%zu", v);
    };
    kv(y, x_l_label,        x_l_value,        "event_count",   static_cast<std::size_t>(d.event_count));
    kv(y, x_l_label + 22,   x_l_value + 22,   "next_order_id", static_cast<std::size_t>(d.next_order_id));
    kv(y, x_r_label,        x_r_value,        "pending_orders",d.pending_orders);
    kv(y, x_r_label + 18,   x_r_value + 16,   "pending_stops", d.pending_stops);
    ++y;
    if (y >= y_end) return;
    kv(y, x_l_label,        x_l_value,        "open_orders",   d.open_orders_cache);
    kv(y, x_l_label + 22,   x_l_value + 22,   "order_meta",    d.order_meta_size);
    kv(y, x_r_label,        x_r_value,        "armed_brackets",d.armed_brackets);
    kv(y, x_r_label + 18,   x_r_value + 16,   "exit_pending",  d.exit_pending);
    ++y;

    // ── Queue modeling (Phase 2 deepdive) ──
    if (y < y_end - 1) {
        section(y++, 0, width, "Queue modeling");
        if (y < y_end) {
            label(y, x_l_label, "avg_pos_bps");
            mvprintw(y, x_l_value, "%u (%u%%)", snap->queue.avg_bps, snap->queue.avg_bps / 100);
            label(y, x_r_label, "submitted");
            mvprintw(y, x_r_value, "%zu", snap->queue.submitted_with_queue);
            ++y;
        }
        if (y < y_end) {
            label(y, x_l_label, "filled_after");
            mvprintw(y, x_l_value, "%zu", snap->queue.filled_after_drain);
            label(y, x_r_label, "blocked_eos");
            mvprintw(y, x_r_value, "%zu", snap->queue.blocked_at_eos);
            ++y;
        }
    }

    // ── Last errors ──
    if (y < y_end - 1) section(y++, 0, width, "Last errors  (per subsystem)");
    if (y >= y_end) return;
    for (const auto& e : d.errors)
    {
        if (y >= y_end) break;
        label(y, x_l_label, e.name);
        if (e.msg.empty()) {
            attron(A_DIM);
            mvaddstr(y, x_l_value, "—");
            attroff(A_DIM);
        } else {
            attron(COLOR_PAIR(kPairRed));
            safe_mvprintw(y, x_l_value, width - x_l_value - 2, "%s", e.msg.c_str());
            attroff(COLOR_PAIR(kPairRed));
        }
        ++y;
    }

    // ── HAS_DEBUG sections (stage timings + memory) ──
    if (y < y_end - 1) section(y++, 0, width, "Stage timings  (HAS_DEBUG only)");
    if (y < y_end)
    {
        if (!d.has_debug)
        {
            attron(A_DIM);
            mvaddstr(y++, x_l_label,
                "(rebuild with -DENABLE_DEBUG=ON to record stage timings)");
            attroff(A_DIM);
        }
        else if (d.stages.empty())
        {
            attron(A_DIM);
            mvaddstr(y++, x_l_label, "(no stage records yet — fire some events first)");
            attroff(A_DIM);
        }
        else
        {
            // Header row.
            attron(A_DIM);
            mvprintw(y, x_l_label, "%-18s %12s %12s %12s %12s",
                     "stage", "calls", "avg", "min", "max");
            attroff(A_DIM);
            ++y;
            auto fmt_ns = [](std::uint64_t ns) -> std::string {
                char b[32];
                if (ns >= 1'000'000)      std::snprintf(b, sizeof(b), "%.2f ms", ns / 1e6);
                else if (ns >= 1'000)     std::snprintf(b, sizeof(b), "%.2f µs", ns / 1e3);
                else                       std::snprintf(b, sizeof(b), "%llu ns", (unsigned long long)ns);
                return b;
            };
            for (const auto& s : d.stages)
            {
                if (y >= y_end) break;
                mvprintw(y, x_l_label, "%-18s %12llu",
                         s.name, (unsigned long long)s.calls);
                mvprintw(y, x_l_label + 32, "%12s", fmt_ns(s.avg_ns).c_str());
                mvprintw(y, x_l_label + 45, "%12s", fmt_ns(s.min_ns).c_str());
                // Max color: yellow > 100µs, red > 1ms.
                int mp = (s.max_ns > 1'000'000) ? kPairRed
                       : (s.max_ns > 100'000)   ? kPairYellow : kPairWhite;
                attron(COLOR_PAIR(mp));
                mvprintw(y, x_l_label + 58, "%12s", fmt_ns(s.max_ns).c_str());
                attroff(COLOR_PAIR(mp));
                ++y;
            }
        }
    }

    if (y < y_end - 1) section(y++, 0, width, "Memory map");
    if (y < y_end)
    {
        const auto& m = snap->memory;
        if (!m.available && m.pool_bytes_total == 0 && m.ring_bytes_total == 0)
        {
            attron(A_DIM);
            mvaddstr(y, x_l_label,
                "(/proc/self/* unavailable on this platform)");
            attroff(A_DIM);
        }
        else
        {
            auto fmt_mb = [](std::uint64_t b) -> std::string {
                char buf[32];
                if (b >= 1024 * 1024)
                    std::snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024.0));
                else if (b >= 1024)
                    std::snprintf(buf, sizeof(buf), "%.1f KB", b / 1024.0);
                else
                    std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
                return buf;
            };

            // ── Process RSS bar (current vs peak) ──
            const int bar_x      = x_l_label + 2;
            const int bar_w      = std::min(width - bar_x - 24, 60);
            const std::uint64_t denom_rss =
                std::max<std::uint64_t>(m.peak_rss_bytes, m.rss_bytes);
            const double rss_frac = denom_rss > 0
                ? static_cast<double>(m.rss_bytes) / denom_rss : 0.0;

            label(y, x_l_label, "RSS");
            char rb[64];
            std::snprintf(rb, sizeof(rb), "%-10s  peak %s",
                          fmt_mb(m.rss_bytes).c_str(),
                          fmt_mb(m.peak_rss_bytes).c_str());
            mvaddstr(y, bar_x + bar_w + 4, rb);
            // Bar
            const int filled = static_cast<int>(std::lround(rss_frac * bar_w));
            int rss_pair = (rss_frac > 0.9) ? kPairRed
                         : (rss_frac > 0.6) ? kPairYellow : kPairGreen;
            mvaddch(y, bar_x, '[');
            attron(COLOR_PAIR(rss_pair));
            for (int i = 0; i < filled; ++i)
                mvaddch(y, bar_x + 1 + i, ACS_CKBOARD);
            attroff(COLOR_PAIR(rss_pair));
            attron(A_DIM);
            for (int i = filled; i < bar_w; ++i)
                mvaddch(y, bar_x + 1 + i, '.');
            attroff(A_DIM);
            mvaddch(y, bar_x + 1 + bar_w, ']');
            ++y;
            if (y >= y_end) return;

            // ── Composition stacked bar: pools | rings | other-rss ──
            const std::uint64_t known = m.pool_bytes_total + m.ring_bytes_total;
            const std::uint64_t other =
                (m.rss_bytes > known) ? (m.rss_bytes - known) : 0;
            const std::uint64_t total = std::max<std::uint64_t>(m.rss_bytes, known);
            const auto frac = [&](std::uint64_t v) {
                return total > 0 ? static_cast<double>(v) / total : 0.0;
            };
            const int w_pools = static_cast<int>(std::lround(frac(m.pool_bytes_total) * bar_w));
            const int w_rings = static_cast<int>(std::lround(frac(m.ring_bytes_total) * bar_w));
            const int w_other = std::max(0, bar_w - w_pools - w_rings);

            label(y, x_l_label, "Composition");
            mvaddch(y, bar_x, '[');
            attron(COLOR_PAIR(kPairCyan));
            for (int i = 0; i < w_pools; ++i)
                mvaddch(y, bar_x + 1 + i, ACS_CKBOARD);
            attroff(COLOR_PAIR(kPairCyan));
            attron(COLOR_PAIR(kPairYellow));
            for (int i = 0; i < w_rings; ++i)
                mvaddch(y, bar_x + 1 + w_pools + i, ACS_CKBOARD);
            attroff(COLOR_PAIR(kPairYellow));
            attron(COLOR_PAIR(kPairWhite));
            for (int i = 0; i < w_other; ++i)
                mvaddch(y, bar_x + 1 + w_pools + w_rings + i, ACS_BOARD);
            attroff(COLOR_PAIR(kPairWhite));
            mvaddch(y, bar_x + 1 + bar_w, ']');
            ++y;
            if (y >= y_end) return;

            // Legend line for the composition bar.
            attron(A_DIM);
            mvaddstr(y, x_l_label, "  ");
            attroff(A_DIM);
            attron(COLOR_PAIR(kPairCyan));
            mvaddch(y, x_l_label + 2, ACS_CKBOARD);
            attroff(COLOR_PAIR(kPairCyan));
            mvprintw(y, x_l_label + 4, "pools %s   ", fmt_mb(m.pool_bytes_total).c_str());
            attron(COLOR_PAIR(kPairYellow));
            mvaddch(y, x_l_label + 22, ACS_CKBOARD);
            attroff(COLOR_PAIR(kPairYellow));
            mvprintw(y, x_l_label + 24, "rings %s   ", fmt_mb(m.ring_bytes_total).c_str());
            attron(COLOR_PAIR(kPairWhite));
            mvaddch(y, x_l_label + 42, ACS_BOARD);
            attroff(COLOR_PAIR(kPairWhite));
            mvprintw(y, x_l_label + 44, "other %s", fmt_mb(other).c_str());
            ++y;
            if (y >= y_end) return;

            // ── Other-segment breakdown (heap / stacks / .so / mmap) ──
            // Sub-stacked bar showing how the "other" RSS slice is
            // composed. Five fixed colors so segments stay visually
            // identifiable across panels.
            if (!m.other_breakdown.empty())
            {
                std::uint64_t other_total = 0;
                for (const auto& seg : m.other_breakdown) other_total += seg.bytes;
                if (other_total > 0)
                {
                    label(y, x_l_label, "  other");
                    mvaddch(y, bar_x, '[');
                    int x_cur = bar_x + 1;
                    const int seg_pairs[] = {
                        kPairCyan, kPairYellow, kPairGreen, kPairWhite, kPairRed
                    };
                    for (std::size_t i = 0; i < m.other_breakdown.size(); ++i)
                    {
                        const auto& seg = m.other_breakdown[i];
                        const double f = static_cast<double>(seg.bytes) / other_total;
                        const int w = std::min(bar_w - (x_cur - bar_x - 1),
                                               static_cast<int>(std::lround(f * bar_w)));
                        const int p = seg_pairs[i % (sizeof(seg_pairs)/sizeof(int))];
                        attron(COLOR_PAIR(p));
                        for (int k = 0; k < w; ++k)
                            mvaddch(y, x_cur + k, ACS_CKBOARD);
                        attroff(COLOR_PAIR(p));
                        x_cur += w;
                    }
                    // Pad remainder if rounding left a gap.
                    attron(A_DIM);
                    for (int k = x_cur; k < bar_x + 1 + bar_w; ++k)
                        mvaddch(y, k, '.');
                    attroff(A_DIM);
                    mvaddch(y, bar_x + 1 + bar_w, ']');
                    ++y;
                    // Legend (one cell per segment, color-keyed).
                    if (y < y_end)
                    {
                        int lx = x_l_label + 2;
                        for (std::size_t i = 0; i < m.other_breakdown.size(); ++i)
                        {
                            const auto& seg = m.other_breakdown[i];
                            const int p = seg_pairs[i % (sizeof(seg_pairs)/sizeof(int))];
                            attron(COLOR_PAIR(p));
                            mvaddch(y, lx, ACS_CKBOARD);
                            attroff(COLOR_PAIR(p));
                            char b[32];
                            std::snprintf(b, sizeof(b), " %s %s   ",
                                          seg.name, fmt_mb(seg.bytes).c_str());
                            mvaddstr(y, lx + 1, b);
                            lx += 1 + static_cast<int>(std::strlen(b));
                        }
                        ++y;
                    }
                }
            }

            // Heap line (data segment from /proc/self/statm).
            if (y < y_end)
            {
                label(y, x_l_label, "Heap (data)");
                mvaddstr(y, bar_x + bar_w + 4, fmt_mb(m.heap_bytes).c_str());
                ++y;
            }


            // ── Per-pool detail ──
            // Bar shows in-use vs capacity (slots), not byte share. The
            // composition bar above already covers byte-share; per-pool
            // utilisation tells you whether the pool is actually carrying
            // load or is mostly idle reserved capacity.
            if (y < y_end - 1) section(y++, 0, width, "Object pools  (live in-use / capacity slots)");
            for (const auto& p : m.pools)
            {
                if (y >= y_end) break;
                mvprintw(y, x_l_label, "%-13s", p.name);
                const double uf = (p.capacity_slots > 0)
                    ? static_cast<double>(p.in_use) / p.capacity_slots : 0.0;
                const int pw = std::min(bar_w,
                    static_cast<int>(std::lround(uf * bar_w)));
                int u_pair = (uf > 0.85) ? kPairRed
                           : (uf > 0.5)  ? kPairYellow
                           : (p.in_use > 0) ? kPairGreen
                           : kPairWhite;
                mvaddch(y, bar_x, '[');
                attron(COLOR_PAIR(u_pair));
                for (int i = 0; i < pw; ++i)
                    mvaddch(y, bar_x + 1 + i, ACS_CKBOARD);
                attroff(COLOR_PAIR(u_pair));
                attron(A_DIM);
                for (int i = pw; i < bar_w; ++i)
                    mvaddch(y, bar_x + 1 + i, '.');
                attroff(A_DIM);
                mvaddch(y, bar_x + 1 + bar_w, ']');
                char d2[96];
                std::snprintf(d2, sizeof(d2),
                              "%5zu / %-7zu  %-10s (%zu blk grow=%zu)",
                              p.in_use, p.capacity_slots,
                              fmt_mb(p.bytes).c_str(),
                              p.blocks, p.grow_count);
                mvaddstr(y, bar_x + bar_w + 4, d2);
                ++y;
            }

            // ── Per-ring detail ──
            if (y < y_end - 1) section(y++, 0, width, "Ring buffers");
            for (const auto& r : m.rings)
            {
                if (y >= y_end) break;
                mvprintw(y, x_l_label, "%-13s", r.name);
                if (r.capacity == 0)
                {
                    attron(A_DIM);
                    for (int i = 0; i < bar_w + 2; ++i)
                        mvaddch(y, bar_x + i, ' ');
                    mvaddstr(y, bar_x + 2, "(not running)");
                    attroff(A_DIM);
                }
                else
                {
                    const double rf = m.ring_bytes_total > 0
                        ? static_cast<double>(r.bytes) / m.ring_bytes_total : 0.0;
                    const int rw = static_cast<int>(std::lround(rf * bar_w));
                    mvaddch(y, bar_x, '[');
                    attron(COLOR_PAIR(kPairYellow));
                    for (int i = 0; i < rw; ++i)
                        mvaddch(y, bar_x + 1 + i, ACS_CKBOARD);
                    attroff(COLOR_PAIR(kPairYellow));
                    attron(A_DIM);
                    for (int i = rw; i < bar_w; ++i)
                        mvaddch(y, bar_x + 1 + i, '.');
                    attroff(A_DIM);
                    mvaddch(y, bar_x + 1 + bar_w, ']');
                    char d2[64];
                    std::snprintf(d2, sizeof(d2), "%-10s  (%zu × %zu B)",
                                  fmt_mb(r.bytes).c_str(), r.capacity, r.element_bytes);
                    mvaddstr(y, bar_x + bar_w + 4, d2);
                }
                ++y;
            }
        }
    }
}

}

#endif // HAS_RICH_TUI
