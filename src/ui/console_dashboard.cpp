#include "ui/console_dashboard.h"
#include "ui/ansi.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>

#include <unistd.h>

namespace truetest::ui {

namespace {

// Visible width of the dashboard box (columns, excluding border).
// Content is rendered into a 70-column inner field, then wrapped with
// "│ " + content + " │" by the row helper. Total box width = 74.
constexpr int inner_width = 70;
constexpr int content_width = inner_width - 2;  // leave 1 space each side

std::string make_hline(int n, const char* middle = "─")
{
    std::string s;
    s.reserve(static_cast<std::size_t>(n) * 3);
    for (int i = 0; i < n; ++i)
        s += middle;
    return s;
}

// Hand-rolled thousands separator — locale-free, allocation-minimal.
std::string fmt_u64(std::uint64_t v)
{
    char raw[24];
    auto res = std::to_chars(raw, raw + sizeof(raw), v);
    std::size_t len = static_cast<std::size_t>(res.ptr - raw);
    std::string out;
    out.reserve(len + len / 3);
    for (std::size_t i = 0; i < len; ++i)
    {
        if (i > 0 && ((len - i) % 3) == 0) out += ',';
        out += raw[i];
    }
    return out;
}

std::string fmt_price_fp8(std::int64_t fp, int decimals = 2)
{
    if (fp < 0) return "—";
    std::uint64_t whole = static_cast<std::uint64_t>(fp) / 100000000ULL;
    std::uint64_t frac  = static_cast<std::uint64_t>(fp) % 100000000ULL;
    // Scale frac to the requested number of decimals.
    std::uint64_t div = 100000000ULL;
    for (int i = 0; i < decimals; ++i) div /= 10;
    std::uint64_t frac_rounded = frac / (div == 0 ? 1 : div);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s.%0*llu",
                  fmt_u64(whole).c_str(),
                  decimals,
                  static_cast<unsigned long long>(frac_rounded));
    return buf;
}

std::string fmt_pnl_fp4(std::int64_t fp4)
{
    bool neg = fp4 < 0;
    std::uint64_t mag = neg ? static_cast<std::uint64_t>(-fp4)
                             : static_cast<std::uint64_t>(fp4);
    std::uint64_t whole = mag / 10000ULL;
    std::uint64_t frac  = mag % 10000ULL / 100ULL;  // 2 decimals
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s$%s.%02llu",
                  neg ? "-" : "+",
                  fmt_u64(whole).c_str(),
                  static_cast<unsigned long long>(frac));
    return buf;
}

std::string fmt_duration(std::chrono::seconds s)
{
    long long total = s.count();
    long long h = total / 3600;
    long long m = (total / 60) % 60;
    long long sec = total % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", h, m, sec);
    return buf;
}

// Best-effort visible width. Counts bytes only — we control the inputs and
// avoid embedding CJK / combining characters, so byte length equals column
// width for everything except UTF-8 multi-byte sequences (box chars, "●").
// Those are all 3-byte / 3-byte sequences rendering to 1 column, so the
// adjustment is: subtract 2 bytes per leading UTF-8 continuation byte.
int visible_width_utf8(std::string_view s)
{
    int w = 0;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) == 0x80) continue;  // continuation byte — no column
        // ANSI CSI sequence — consume until the final byte (0x40-0x7E).
        if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[')
        {
            i += 1;  // skip '['
            while (i + 1 < s.size())
            {
                unsigned char next = static_cast<unsigned char>(s[++i]);
                if (next >= 0x40 && next <= 0x7E) break;
            }
            continue;
        }
        ++w;
    }
    return w;
}

std::string pad_right(const std::string& s, int target_cols)
{
    int vis = visible_width_utf8(s);
    if (vis >= target_cols) return s;
    return s + std::string(static_cast<std::size_t>(target_cols - vis), ' ');
}

// Build one box row, including the trailing clear-to-eol so any leftover
// bytes from a previously-longer value get wiped when this row is emitted.
// No trailing newline — the diff loop appends '\n' (or skips the row and
// emits a cursor-down instead) so the cursor advances exactly one line per
// row regardless of whether the row was rewritten.
std::string row(const std::string& content_with_ansi, bool color_on)
{
    std::string padded = pad_right(content_with_ansi, content_width);
    std::string out;
    out.reserve(padded.size() + 12);
    out += "│ ";
    out += padded;
    out += " │";
    out += ansi::clear_to_eol;
    (void)color_on;
    return out;
}

const char* severity_color(event_severity s, bool on)
{
    if (!on) return "";
    switch (s)
    {
        case event_severity::error:  return ansi::fg_br_red;
        case event_severity::warn:   return ansi::fg_br_yel;
        case event_severity::notice: return ansi::fg_cyan;
        default:                     return ansi::fg_gray;
    }
}

const char* severity_label(event_severity s)
{
    switch (s)
    {
        case event_severity::error:  return "ERROR";
        case event_severity::warn:   return "WARN ";
        case event_severity::notice: return "INFO ";
        default:                     return "     ";
    }
}

std::string_view state_label(connection_state s)
{
    switch (s)
    {
        case connection_state::idle:         return "IDLE";
        case connection_state::backfill:     return "BACKFILL";
        case connection_state::waiting:      return "WAITING";
        case connection_state::live:         return "LIVE";
        case connection_state::reconnecting: return "RECONNECT";
        case connection_state::halted:       return "HALTED";
        case connection_state::closed:       return "CLOSED";
    }
    return "?";
}

const char* state_color(connection_state s, bool on)
{
    if (!on) return "";
    switch (s)
    {
        case connection_state::live:         return ansi::fg_br_green;
        case connection_state::backfill:
        case connection_state::waiting:      return ansi::fg_br_yel;
        case connection_state::halted:
        case connection_state::reconnecting: return ansi::fg_br_red;
        default:                             return ansi::fg_gray;
    }
}

void write_all(int fd, std::string_view s)
{
    const char* p = s.data();
    std::size_t left = s.size();
    while (left > 0)
    {
        ssize_t n = ::write(fd, p, left);
        if (n <= 0)
        {
            if (n < 0 && (errno == EINTR)) continue;
            break;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
}

std::string clock_hhmmss(std::chrono::system_clock::time_point tp)
{
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buf;
}

} // namespace

bool ConsoleDashboard::stdout_is_tty()
{
    return ::isatty(STDOUT_FILENO) != 0;
}

bool ConsoleDashboard::supports_color()
{
    if (!stdout_is_tty()) return false;
    const char* term = std::getenv("TERM");
    if (!term) return false;
    if (std::strcmp(term, "dumb") == 0) return false;
    if (std::getenv("NO_COLOR") != nullptr) return false;
    return true;
}

output_mode ConsoleDashboard::resolve_mode(output_mode requested)
{
    if (requested != output_mode::auto_detect) return requested;
    if (stdout_is_tty() && supports_color()) return output_mode::tui;
    return output_mode::plain;
}

ConsoleDashboard::ConsoleDashboard(dashboard_config cfg)
    : cfg_(std::move(cfg))
    , resolved_mode_(resolve_mode(cfg_.mode))
{
}

ConsoleDashboard::~ConsoleDashboard()
{
    stop();
}

void ConsoleDashboard::start()
{
    if (resolved_mode_ == output_mode::off) return;
    if (running_.exchange(true)) return;  // already running

    start_time_ = std::chrono::steady_clock::now();
    last_sample_time_ = start_time_;
    last_sample_events_ = 0;

    if (resolved_mode_ == output_mode::tui)
    {
        // Hide cursor while the dashboard owns the bottom of the terminal.
        // No alt-screen — we want the banner and prior scrollback to stay.
        write_all(STDOUT_FILENO, ansi::cursor_hide);
    }

    thread_ = std::thread([this] { render_loop(); });
}

void ConsoleDashboard::stop()
{
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();

    if (resolved_mode_ == output_mode::tui)
    {
        // Final frame, then show cursor and emit a newline so the shell
        // prompt lands on a fresh line rather than on top of the box.
        std::string buf;
        render_tui(buf);
        write_all(STDOUT_FILENO, buf);
        write_all(STDOUT_FILENO, ansi::cursor_show);
        write_all(STDOUT_FILENO, "\n");
    }
}

void ConsoleDashboard::set_state(connection_state s)
{
    stats_.state.store(static_cast<std::uint8_t>(s), std::memory_order_release);
}

void ConsoleDashboard::set_feed_label(std::string label)
{
    cfg_.feed = std::move(label);
}

void ConsoleDashboard::push_event(event_severity sev, std::string_view msg)
{
    std::lock_guard<std::mutex> lk(recent_mu_);
    std::uint64_t idx = recent_head_.fetch_add(1, std::memory_order_acq_rel);
    auto& slot = recent_[idx % recent_cap];
    slot.ts = std::chrono::system_clock::now();
    slot.sev = sev;
    slot.msg.assign(msg);
}

void ConsoleDashboard::render_banner()
{
    if (resolved_mode_ == output_mode::off) return;
    if (resolved_mode_ != output_mode::tui)
    {
        std::string buf;
        buf += "[truetest] ";
        buf += cfg_.title;
        buf += " • ";
        buf += cfg_.target;
        if (!cfg_.feed.empty())
        {
            buf += " • ";
            buf += cfg_.feed;
        }
        buf += "\n";
        write_all(STDOUT_FILENO, buf);
    }
    // In TUI mode the live dashboard is the banner — nothing to print here.
}

void ConsoleDashboard::update_rate_ema(std::uint64_t now_events,
                                       std::chrono::steady_clock::time_point now)
{
    auto dt = std::chrono::duration<double>(now - last_sample_time_).count();
    if (dt < 1e-6) return;
    std::uint64_t delta = now_events - last_sample_events_;
    double inst = static_cast<double>(delta) / dt;
    constexpr double alpha = 0.25;
    rate_ema_ = (rate_ema_ == 0.0) ? inst : alpha * inst + (1.0 - alpha) * rate_ema_;
    last_sample_time_ = now;
    last_sample_events_ = now_events;
}

void ConsoleDashboard::render_loop()
{
    std::string buf;
    buf.reserve(4096);

    while (running_.load(std::memory_order_acquire))
    {
        auto now = std::chrono::steady_clock::now();
        update_rate_ema(stats_.events_total.load(std::memory_order_relaxed), now);

        buf.clear();
        switch (resolved_mode_)
        {
            case output_mode::tui:   render_tui(buf);   break;
            case output_mode::plain: render_plain(buf); break;
            case output_mode::ndjson: render_ndjson(buf); break;
            default: break;
        }
        write_all(STDOUT_FILENO, buf);

        std::this_thread::sleep_for(cfg_.render_interval);
    }
}

void ConsoleDashboard::render_tui(std::string& buf)
{
    const bool color = supports_color();
    const auto state = static_cast<connection_state>(
        stats_.state.load(std::memory_order_acquire));

    // Counters (all relaxed — display-only).
    const std::uint64_t events = stats_.events_total.load(std::memory_order_relaxed);
    const std::uint64_t fills  = stats_.fills_total.load(std::memory_order_relaxed);
    const std::uint64_t trades = stats_.trades_total.load(std::memory_order_relaxed);
    const std::int64_t last_px = stats_.last_price_fp8.load(std::memory_order_relaxed);
    const std::int64_t bid     = stats_.best_bid_fp8.load(std::memory_order_relaxed);
    const std::int64_t ask     = stats_.best_ask_fp8.load(std::memory_order_relaxed);
    const std::uint32_t bf_done  = stats_.backfill_done.load(std::memory_order_relaxed);
    const std::uint32_t bf_total = stats_.backfill_total.load(std::memory_order_relaxed);
    const std::uint32_t open_ord = stats_.open_orders.load(std::memory_order_relaxed);
    const std::uint64_t drops = stats_.ring_drops_logging.load(std::memory_order_relaxed)
                              + stats_.ring_drops_risk.load(std::memory_order_relaxed)
                              + stats_.ring_drops_stats.load(std::memory_order_relaxed)
                              + stats_.ring_drops_observer.load(std::memory_order_relaxed)
                              + stats_.ring_drops_risk_stats.load(std::memory_order_relaxed)
                              + stats_.ring_drops_mm.load(std::memory_order_relaxed);
    const std::int64_t pnl_fp4 = stats_.realized_pnl_fp4.load(std::memory_order_relaxed);
    const std::int64_t dd_fp4  = stats_.drawdown_fp4.load(std::memory_order_relaxed);
    const std::uint32_t wr_bps = stats_.win_rate_bps.load(std::memory_order_relaxed);
    const bool halted = stats_.halt_flag.load(std::memory_order_acquire);

    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time_);

    // Build every row's bytes into rows[]. Each entry is a self-contained
    // line (no trailing '\n'); the diff loop below appends '\n' or skips
    // the row and emits a cursor-down instead. Doing this means cells
    // whose values haven't changed don't get rewritten on every tick, so
    // the box stops flickering when only a handful of values are moving.
    std::vector<std::string> rows;
    rows.reserve(14);

    // Top border with title + uptime.
    {
        std::string title;
        title += cfg_.title;
        title += " • ";
        title += cfg_.target;
        if (!cfg_.feed.empty())
        {
            title += " • ";
            title += cfg_.feed;
        }
        std::string time_str = fmt_duration(uptime);
        int title_vis = visible_width_utf8(title) + visible_width_utf8(time_str) + 5;
        int dashes = inner_width - title_vis;
        if (dashes < 2) dashes = 2;
        std::string r;
        r += "┌─ ";
        r += title;
        r += ' ';
        r += make_hline(dashes);
        r += ' ';
        r += time_str;
        r += " ─┐";
        r += ansi::clear_to_eol;
        rows.push_back(std::move(r));
    }

    // State / Rate / Ring drops.
    {
        std::string c;
        c += "State  ";
        if (color) c += state_color(state, true);
        c += "● ";
        c += state_label(state);
        if (color) c += ansi::reset;
        c = pad_right(c, 22);
        c += "Feed  ";
        char rb[32];
        std::snprintf(rb, sizeof(rb), "%6.0f ev/s", rate_ema_);
        c += rb;
        c = pad_right(c, 48);
        c += "Ring drops ";
        c += fmt_u64(drops);
        rows.push_back(row(c, color));
    }

    // Last / Spread / Backfill.
    {
        std::string c;
        c += "Last   ";
        c += last_px < 0 ? "—" : fmt_price_fp8(last_px);
        c = pad_right(c, 22);
        c += "Spread ";
        if (bid > 0 && ask > 0)
        {
            double b = static_cast<double>(bid) / 1e8;
            double a = static_cast<double>(ask) / 1e8;
            double mid = (b + a) * 0.5;
            double bps = mid > 0 ? (a - b) / mid * 1e4 : 0.0;
            char sb[32];
            std::snprintf(sb, sizeof(sb), "%5.2f bps", bps);
            c += sb;
        }
        else
        {
            c += "—";
        }
        c = pad_right(c, 48);
        c += "Backfill ";
        if (bf_total > 0)
        {
            char b[32];
            std::snprintf(b, sizeof(b), "%u/%u %s", bf_done, bf_total,
                          bf_done >= bf_total ? "✓" : "…");
            c += b;
        }
        else
        {
            c += "—";
        }
        rows.push_back(row(c, color));
    }

    // Separator.
    {
        std::string r;
        r += "├";
        r += make_hline(inner_width);
        r += "┤";
        r += ansi::clear_to_eol;
        rows.push_back(std::move(r));
    }

    // Events / Fills / Round-trips.
    {
        std::string c;
        c += "Events  ";
        c += fmt_u64(events);
        c = pad_right(c, 26);
        c += "Fills ";
        c += fmt_u64(fills);
        c = pad_right(c, 40);
        c += "Round-trips ";
        c += fmt_u64(trades);
        rows.push_back(row(c, color));
    }

    // PnL / Drawdown / Win rate.
    {
        std::string c;
        c += "Realized PnL  ";
        c += fmt_pnl_fp4(pnl_fp4);
        c = pad_right(c, 30);
        c += "Drawdown ";
        char db[32];
        std::snprintf(db, sizeof(db), "%+.2f%%",
                      static_cast<double>(dd_fp4) / 1e2);
        c += db;
        c = pad_right(c, 50);
        c += "Win rate ";
        char wr[16];
        std::snprintf(wr, sizeof(wr), "%u%%", wr_bps / 100);
        c += wr;
        rows.push_back(row(c, color));
    }

    // Risk.
    {
        std::string c;
        c += "Risk    halt:";
        if (color) c += halted ? ansi::fg_br_red : ansi::fg_gray;
        c += halted ? "yes" : "no";
        if (color) c += ansi::reset;
        c += "  open:";
        c += fmt_u64(open_ord);
        if (cfg_.risk_max_open_orders > 0)
        {
            c += "/";
            c += fmt_u64(static_cast<std::uint64_t>(cfg_.risk_max_open_orders));
        }
        if (cfg_.risk_max_daily_loss > 0.0)
        {
            char buf2[48];
            std::snprintf(buf2, sizeof(buf2), "  day-loss cap $%.0f",
                          cfg_.risk_max_daily_loss);
            c += buf2;
        }
        rows.push_back(row(c, color));
    }

    // Rings.
    {
        std::string c;
        c += "Rings   log:";
        c += fmt_u64(stats_.ring_drops_logging.load(std::memory_order_relaxed));
        c += "  risk:";
        c += fmt_u64(stats_.ring_drops_risk.load(std::memory_order_relaxed));
        c += "  stats:";
        c += fmt_u64(stats_.ring_drops_stats.load(std::memory_order_relaxed));
        c += "  mm:";
        c += fmt_u64(stats_.ring_drops_mm.load(std::memory_order_relaxed));
        rows.push_back(row(c, color));
    }

    // Recent events pane header.
    {
        std::string r;
        r += "├─ recent ";
        r += make_hline(inner_width - 9);
        r += "┤";
        r += ansi::clear_to_eol;
        rows.push_back(std::move(r));
    }

    // Last 4 events, oldest → newest.
    constexpr int recent_rows = 4;
    event_entry snapshot[recent_rows];
    int have = 0;
    {
        std::lock_guard<std::mutex> lk(recent_mu_);
        std::uint64_t head = recent_head_.load(std::memory_order_relaxed);
        int start = static_cast<int>((std::min)(static_cast<std::uint64_t>(recent_rows), head));
        for (int i = start; i > 0; --i)
        {
            std::uint64_t idx = head - static_cast<std::uint64_t>(i);
            snapshot[have++] = recent_[idx % recent_cap];
        }
    }
    for (int i = 0; i < recent_rows; ++i)
    {
        std::string c;
        if (i < have)
        {
            c += clock_hhmmss(snapshot[i].ts);
            c += "  ";
            if (color) c += severity_color(snapshot[i].sev, true);
            c += severity_label(snapshot[i].sev);
            if (color) c += ansi::reset;
            c += "  ";
            c += snapshot[i].msg;
        }
        rows.push_back(row(c, color));
    }

    // Bottom border.
    {
        std::string r;
        r += "└";
        r += make_hline(inner_width);
        r += "┘";
        r += ansi::clear_to_eol;
        rows.push_back(std::move(r));
    }

    const int row_count = static_cast<int>(rows.size());

    // First frame, or a row-count change (shouldn't happen — height is
    // constant 14 — but guard anyway): full repaint, then seed the cache.
    if (last_row_count_ == 0 || last_row_count_ != row_count
        || static_cast<int>(last_rows_.size()) != row_count)
    {
        if (last_row_count_ > 0)
        {
            char up[16];
            std::snprintf(up, sizeof(up), "\x1b[%dA", last_row_count_);
            buf += up;
            buf += ansi::erase_down;
        }
        buf += '\r';
        for (const auto& r : rows)
        {
            buf += r;
            buf += '\n';
        }
        last_rows_ = rows;
        last_row_count_ = row_count;
        return;
    }

    // Steady-state path: hop to the top of the box, then per-row diff.
    // Unchanged rows just advance the cursor (ESC[B + CR) so the border
    // and unchanging cells emit zero bytes — which is what stops the box
    // from flickering when the only thing moving is a couple of counters.
    char up[16];
    std::snprintf(up, sizeof(up), "\x1b[%dA", last_row_count_);
    buf += up;
    buf += '\r';
    for (int i = 0; i < row_count; ++i)
    {
        if (rows[i] == last_rows_[i])
        {
            buf += "\x1b[B\r";
        }
        else
        {
            buf += rows[i];
            buf += '\n';
            last_rows_[i] = rows[i];
        }
    }
}

void ConsoleDashboard::render_plain(std::string& buf)
{
    const std::uint64_t events = stats_.events_total.load(std::memory_order_relaxed);
    const std::uint64_t fills  = stats_.fills_total.load(std::memory_order_relaxed);
    const std::uint64_t trades = stats_.trades_total.load(std::memory_order_relaxed);
    const auto state = static_cast<connection_state>(
        stats_.state.load(std::memory_order_relaxed));

    char line[256];
    std::snprintf(line, sizeof(line),
                  "[%s] events=%llu fills=%llu trades=%llu rate=%.0f/s\n",
                  std::string(state_label(state)).c_str(),
                  static_cast<unsigned long long>(events),
                  static_cast<unsigned long long>(fills),
                  static_cast<unsigned long long>(trades),
                  rate_ema_);
    buf += line;
}

void ConsoleDashboard::render_ndjson(std::string& buf)
{
    const std::uint64_t events = stats_.events_total.load(std::memory_order_relaxed);
    const std::uint64_t fills  = stats_.fills_total.load(std::memory_order_relaxed);
    const std::uint64_t trades = stats_.trades_total.load(std::memory_order_relaxed);
    const auto state = static_cast<connection_state>(
        stats_.state.load(std::memory_order_relaxed));

    char line[256];
    std::snprintf(line, sizeof(line),
                  "{\"state\":\"%s\",\"events\":%llu,\"fills\":%llu,"
                  "\"trades\":%llu,\"rate\":%.0f}\n",
                  std::string(state_label(state)).c_str(),
                  static_cast<unsigned long long>(events),
                  static_cast<unsigned long long>(fills),
                  static_cast<unsigned long long>(trades),
                  rate_ema_);
    buf += line;
}

void ConsoleDashboard::print_summary(std::uint64_t events,
                                     std::uint64_t trades,
                                     std::int64_t elapsed_ms)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "Streaming complete: %llu events, %llu trades in %lld ms\n",
                  static_cast<unsigned long long>(events),
                  static_cast<unsigned long long>(trades),
                  static_cast<long long>(elapsed_ms));
    write_all(STDOUT_FILENO, buf);
}

} // namespace truetest::ui
