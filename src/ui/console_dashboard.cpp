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

// Content rendered into 70-col inner field, wrapped to a 74-col box.
constexpr int inner_width = 70;
constexpr int content_width = inner_width - 2;

std::string make_hline(int n, const char* middle = "─")
{
    std::string s;
    s.reserve(static_cast<std::size_t>(n) * 3);
    for (int i = 0; i < n; ++i)
        s += middle;
    return s;
}

// Locale-free thousands separator.
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
    if (fp < 0) return "-";
    std::uint64_t whole = static_cast<std::uint64_t>(fp) / 100000000ULL;
    std::uint64_t frac  = static_cast<std::uint64_t>(fp) % 100000000ULL;
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
    std::uint64_t frac  = mag % 10000ULL / 100ULL;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s$%s.%02llu",
                  neg ? "-" : "+",
                  fmt_u64(whole).c_str(),
                  static_cast<unsigned long long>(frac));
    return buf;
}

// samples==0 renders as em-dash so "0.00 bps" isn't confused with "no data yet".
std::string fmt_toxicity_bps_fp2(std::int32_t fp2, std::uint32_t samples)
{
    if (samples == 0) return "-";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.2f bps",
                  static_cast<double>(fp2) / 100.0);
    return buf;
}

std::string fmt_position_fp8(std::int64_t fp8)
{
    if (fp8 == 0) return "flat";
    const bool neg = fp8 < 0;
    const std::uint64_t mag = neg ? static_cast<std::uint64_t>(-fp8)
                                  : static_cast<std::uint64_t>(fp8);
    const std::uint64_t whole = mag / 100000000ULL;
    const std::uint64_t frac  = (mag % 100000000ULL) / 10000ULL;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s %s.%04llu",
                  neg ? "short" : "long",
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

// Counts bytes and treats UTF-8 continuations + ANSI CSI sequences as zero
// columns. Inputs are controlled - no CJK/combining chars - so this is exact.
int visible_width_utf8(std::string_view s)
{
    int w = 0;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) == 0x80) continue;
        if (c == 0x1B && i + 1 < s.size() && s[i + 1] == '[')
        {
            i += 1;
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

// Trailing clear-to-eol wipes leftovers from a previously-longer value. No
// newline - the diff loop decides whether to emit '\n' or a cursor-down.
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

}

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
    rows_scratch_.reserve(16);

    row_separator_.reserve(inner_width * 3 + 8);
    row_separator_ += "├";
    row_separator_ += make_hline(inner_width);
    row_separator_ += "┤";
    row_separator_ += ansi::clear_to_eol;

    row_bottom_.reserve(inner_width * 3 + 8);
    row_bottom_ += "└";
    row_bottom_ += make_hline(inner_width);
    row_bottom_ += "┘";
    row_bottom_ += ansi::clear_to_eol;

    row_recent_header_.reserve(inner_width * 3 + 16);
    row_recent_header_ += "├─ recent ";
    row_recent_header_ += make_hline(inner_width - 9);
    row_recent_header_ += "┤";
    row_recent_header_ += ansi::clear_to_eol;
}

ConsoleDashboard::~ConsoleDashboard()
{
    stop();
}

void ConsoleDashboard::start()
{
    if (resolved_mode_ == output_mode::off) return;
    if (running_.exchange(true)) return;

    start_time_ = std::chrono::steady_clock::now();
    last_sample_time_ = start_time_;
    last_sample_events_ = 0;

    if (resolved_mode_ == output_mode::tui)
    {
        // No alt-screen so the banner and prior scrollback stay visible.
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
        // Final frame + newline so the shell prompt lands below the box.
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

void ConsoleDashboard::set_shutdown_reason(std::string_view msg)
{
    auto& st = stats_;
    const std::uint64_t cur = st.shutdown_reason_seq.load(std::memory_order_relaxed);

    st.shutdown_reason_seq.store(cur + 1, std::memory_order_release);

    const std::size_t n = (std::min)(msg.size(), streaming_stats::shutdown_reason_cap);
    if (n > 0)
        std::memcpy(st.shutdown_reason_buf, msg.data(), n);
    st.shutdown_reason_len = static_cast<std::uint8_t>(n);

    st.shutdown_reason_seq.store(cur + 2, std::memory_order_release);
}

std::string ConsoleDashboard::shutdown_reason() const
{
    const auto& st = stats_;
    for (int retry = 0; retry < 4; ++retry)
    {
        const std::uint64_t s1 = st.shutdown_reason_seq.load(std::memory_order_acquire);
        if (s1 & 1ULL) continue;
        const std::uint8_t len = st.shutdown_reason_len;
        char buf[streaming_stats::shutdown_reason_cap];
        if (len > 0)
            std::memcpy(buf, st.shutdown_reason_buf, len);
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint64_t s2 = st.shutdown_reason_seq.load(std::memory_order_relaxed);
        if (s1 == s2)
            return std::string(buf, len);
    }
    return {};
}

void ConsoleDashboard::push_event(event_severity sev, std::string_view msg)
{
    // MPSC seqlock: idx*2+1 marks the slot mid-write, idx*2+2 publishes it.
    // Release on the "writing" tag pairs with the reader's acquire so a
    // reader that sees it also sees the prior publication and can skip.
    const std::uint64_t idx = recent_head_.fetch_add(1, std::memory_order_acq_rel);
    auto& slot = recent_[idx % recent_cap];

    slot.seq.store(idx * 2 + 1, std::memory_order_release);

    slot.entry.ts = std::chrono::system_clock::now();
    slot.entry.sev = sev;
    const std::size_t n = (std::min)(msg.size(), recent_msg_cap);
    if (n > 0)
        std::memcpy(slot.entry.msg, msg.data(), n);
    slot.entry.msg_len = static_cast<std::uint8_t>(n);

    slot.seq.store(idx * 2 + 2, std::memory_order_release);
}

std::vector<ConsoleDashboard::recent_event_view>
ConsoleDashboard::recent_events_snapshot(std::size_t max_count) const
{
    std::vector<recent_event_view> out;
    if (max_count == 0) return out;

    const std::uint64_t head = recent_head_.load(std::memory_order_acquire);
    const std::uint64_t take = (std::min)(static_cast<std::uint64_t>(max_count),
                                          head);
    out.reserve(take);
    for (std::uint64_t i = take; i > 0; --i)
    {
        const std::uint64_t idx      = head - i;
        const std::uint64_t expected = idx * 2 + 2;
        const auto& slot = recent_[idx % recent_cap];

        const std::uint64_t s1 = slot.seq.load(std::memory_order_acquire);
        if (s1 != expected) continue;
        event_entry tmp = slot.entry;
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint64_t s2 = slot.seq.load(std::memory_order_relaxed);
        if (s2 != expected) continue;

        recent_event_view v;
        v.ts  = tmp.ts;
        v.sev = tmp.sev;
        v.msg.assign(tmp.msg, tmp.msg_len);
        out.push_back(std::move(v));
    }
    return out;
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

    // Push the smoothed rate into the rolling history ring. One sample
    // per render tick is enough resolution for a 60-cell sparkline.
    rate_history_[rate_history_head_] = rate_ema_;
    rate_history_head_ = (rate_history_head_ + 1) % rate_history_cap;
    if (rate_history_count_ < rate_history_cap) ++rate_history_count_;
}

std::vector<double> ConsoleDashboard::rate_tail(std::size_t n) const
{
    std::vector<double> out;
    if (n == 0 || rate_history_count_ == 0) return out;

    const std::size_t take = std::min(n, rate_history_count_);
    out.reserve(take);

    // Walk back from head_ by `take` positions; head_ points at the
    // next slot to write, so the most recent value is at (head-1) mod cap.
    const std::size_t start = (rate_history_head_ + rate_history_cap - take)
                              % rate_history_cap;
    for (std::size_t i = 0; i < take; ++i)
    {
        const std::size_t idx = (start + i) % rate_history_cap;
        out.push_back(rate_history_[idx]);
    }
    return out;
}

void ConsoleDashboard::render_loop()
{
    std::string buf;
    buf.reserve(4096);

    while (running_.load(std::memory_order_acquire))
    {
        auto now = std::chrono::steady_clock::now();
        update_rate_ema(stats_.events_total.load(std::memory_order_relaxed), now);

        // TUI frame-skip: if the digest of displayed atomics + uptime-sec
        // is unchanged, skip the render and the stdout write entirely.
        if (resolved_mode_ == output_mode::tui)
        {
            const std::uint64_t digest =
                stats_.events_total.load(std::memory_order_relaxed)
              + stats_.fills_total.load(std::memory_order_relaxed)
              + stats_.trades_total.load(std::memory_order_relaxed)
              + recent_head_.load(std::memory_order_relaxed)
              + stats_.state.load(std::memory_order_relaxed)
              + (stats_.halt_flag.load(std::memory_order_relaxed) ? 1ull : 0ull)
              + stats_.open_orders.load(std::memory_order_relaxed)
              + static_cast<std::uint64_t>(
                    stats_.realized_pnl_fp4.load(std::memory_order_relaxed))
              + static_cast<std::uint64_t>(
                    stats_.unrealized_pnl_fp4.load(std::memory_order_relaxed))
              + static_cast<std::uint64_t>(
                    stats_.position_qty_fp8.load(std::memory_order_relaxed))
              + static_cast<std::uint64_t>(
                    stats_.drawdown_fp4.load(std::memory_order_relaxed))
              + stats_.win_rate_bps.load(std::memory_order_relaxed)
              + static_cast<std::uint64_t>(
                    stats_.toxicity_bps_fp2.load(std::memory_order_relaxed))
              + stats_.toxicity_samples.load(std::memory_order_relaxed)
              + stats_.live_quotes.load(std::memory_order_relaxed)
              + stats_.avg_queue_pos_bps.load(std::memory_order_relaxed)
              + static_cast<std::uint64_t>(
                    stats_.best_bid_fp8.load(std::memory_order_relaxed))
              + static_cast<std::uint64_t>(
                    stats_.best_ask_fp8.load(std::memory_order_relaxed))
              + stats_.ring_drops_logging.load(std::memory_order_relaxed)
              + stats_.ring_drops_risk.load(std::memory_order_relaxed)
              + stats_.ring_drops_stats.load(std::memory_order_relaxed)
              + stats_.ring_drops_observer.load(std::memory_order_relaxed)
              + stats_.ring_drops_risk_stats.load(std::memory_order_relaxed)
              + stats_.ring_drops_mm.load(std::memory_order_relaxed);
            const int uptime_sec = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    now - start_time_).count());

            if (have_digest_
                && digest == last_digest_
                && uptime_sec == last_uptime_sec_)
            {
                std::this_thread::sleep_for(cfg_.render_interval);
                continue;
            }
            last_digest_ = digest;
            last_uptime_sec_ = uptime_sec;
            have_digest_ = true;
        }

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
    const std::int64_t pnl_fp4     = stats_.realized_pnl_fp4.load(std::memory_order_relaxed);
    const std::int64_t unreal_fp4  = stats_.unrealized_pnl_fp4.load(std::memory_order_relaxed);
    const std::int64_t pos_qty_fp8 = stats_.position_qty_fp8.load(std::memory_order_relaxed);
    const std::int64_t dd_fp4      = stats_.drawdown_fp4.load(std::memory_order_relaxed);
    const std::uint32_t wr_bps     = stats_.win_rate_bps.load(std::memory_order_relaxed);
    const std::int32_t tox_fp2     = stats_.toxicity_bps_fp2.load(std::memory_order_relaxed);
    const std::uint32_t tox_n      = stats_.toxicity_samples.load(std::memory_order_relaxed);
    const std::uint32_t live_qs    = stats_.live_quotes.load(std::memory_order_relaxed);
    const std::uint32_t qpos_bps   = stats_.avg_queue_pos_bps.load(std::memory_order_relaxed);
    const bool halted = stats_.halt_flag.load(std::memory_order_acquire);

    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time_);

    // Each row is self-contained (no '\n'); the diff loop below decides
    // whether to emit the row or advance the cursor past an unchanged one.
    auto& rows = rows_scratch_;
    rows.clear();

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
        if (color) r += ansi::bold;
        if (color) r += ansi::fg_cyan;
        r += title;
        if (color) r += ansi::reset;
        r += ' ';
        r += make_hline(dashes);
        r += ' ';
        if (color) r += ansi::dim;
        r += time_str;
        if (color) r += ansi::reset;
        r += " ─┐";
        r += ansi::clear_to_eol;
        rows.push_back(std::move(r));
    }

    {
        std::string c;
        if (color) c += ansi::fg_cyan;
        c += "State  ";
        if (color) c += ansi::reset;
        if (color) c += state_color(state, true);
        c += "● ";
        c += state_label(state);
        if (color) c += ansi::reset;
        c = pad_right(c, 22);
        if (color) c += ansi::fg_cyan;
        c += "Feed  ";
        if (color) c += ansi::reset;
        char rb[32];
        std::snprintf(rb, sizeof(rb), "%6.0f ev/s", rate_ema_);
        c += rb;
        c = pad_right(c, 48);
        if (color) c += ansi::fg_cyan;
        c += "Ring drops ";
        if (color) c += (drops > 0 ? ansi::fg_br_red : ansi::reset);
        c += fmt_u64(drops);
        if (color) c += ansi::reset;
        rows.push_back(row(c, color));
    }

    {
        std::string c;
        if (color) c += ansi::fg_cyan;
        c += "Last   ";
        if (color) c += ansi::reset;
        c += last_px < 0 ? "-" : fmt_price_fp8(last_px);
        c = pad_right(c, 22);
        if (color) c += ansi::fg_cyan;
        c += "Spread ";
        if (color) c += ansi::reset;
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
            c += "-";
        }
        c = pad_right(c, 48);
        if (color) c += ansi::fg_cyan;
        c += "Backfill ";
        if (color) c += ansi::reset;
        if (bf_total > 0)
        {
            char b[32];
            std::snprintf(b, sizeof(b), "%u/%u %s", bf_done, bf_total,
                          bf_done >= bf_total ? "✓" : "…");
            c += b;
        }
        else
        {
            c += "-";
        }
        rows.push_back(row(c, color));
    }

    if (!cfg_.threading_summary.empty())
    {
        std::string c;
        if (color) c += ansi::fg_cyan;
        c += "Threads ";
        if (color) c += ansi::reset;
        c += " ";
        if (color) c += ansi::dim;
        c += cfg_.threading_summary;
        if (color) c += ansi::reset;
        rows.push_back(row(c, color));
    }

    rows.push_back(row_separator_);

    {
        std::string c;
        if (color) c += ansi::fg_cyan;
        c += "Events  ";
        if (color) c += ansi::reset;
        c += fmt_u64(events);
        c = pad_right(c, 26);
        if (color) c += ansi::fg_cyan;
        c += "Fills ";
        if (color) c += ansi::reset;
        c += fmt_u64(fills);
        c = pad_right(c, 40);
        if (color) c += ansi::fg_cyan;
        c += "Round-trips ";
        if (color) c += ansi::reset;
        c += fmt_u64(trades);
        rows.push_back(row(c, color));
    }

    {
        std::string c;
        if (color) c += ansi::fg_cyan;
        c += "Realized PnL  ";
        if (color) c += ansi::reset;
        if (color)
            c += pnl_fp4 > 0 ? ansi::fg_br_green
               : pnl_fp4 < 0 ? ansi::fg_br_red
               : ansi::fg_gray;
        c += fmt_pnl_fp4(pnl_fp4);
        if (color) c += ansi::reset;
        c = pad_right(c, 30);
        if (color) c += ansi::fg_cyan;
        c += "Drawdown ";
        if (color) c += ansi::reset;
        char db[32];
        std::snprintf(db, sizeof(db), "%+.2f%%",
                      static_cast<double>(dd_fp4) / 1e2);
        if (color) c += dd_fp4 <= -500 ? ansi::fg_br_red
                     :  dd_fp4 <= -100 ? ansi::fg_br_yel
                     :  ansi::fg_gray;
        c += db;
        if (color) c += ansi::reset;
        rows.push_back(row(c, color));
    }

    {
        std::string c;
        if (color) c += ansi::fg_cyan;
        c += "Unrealized    ";
        if (color) c += ansi::reset;
        if (color)
            c += unreal_fp4 > 0 ? ansi::fg_br_green
               : unreal_fp4 < 0 ? ansi::fg_br_red
               : ansi::fg_gray;
        c += fmt_pnl_fp4(unreal_fp4);
        if (color) c += ansi::reset;
        c = pad_right(c, 30);
        if (color) c += ansi::fg_cyan;
        c += "Position ";
        if (color) c += ansi::reset;
        if (color)
            c += pos_qty_fp8 > 0 ? ansi::fg_br_green
               : pos_qty_fp8 < 0 ? ansi::fg_br_red
               : ansi::fg_gray;
        c += fmt_position_fp8(pos_qty_fp8);
        if (color) c += ansi::reset;
        rows.push_back(row(c, color));
    }

    // Toxicity + Win rate. Toxicity: positive markout = bleeding to adverse
    // selection; red >2 bps, yellow >0.5 bps, gray otherwise.
    {
        std::string c;
        if (color) c += ansi::fg_cyan;
        c += "Toxicity      ";
        if (color) c += ansi::reset;
        if (color && tox_n > 0)
            c += tox_fp2 >  200 ? ansi::fg_br_red
               : tox_fp2 >   50 ? ansi::fg_br_yel
               : tox_fp2 <    0 ? ansi::fg_br_green
               : ansi::fg_gray;
        c += fmt_toxicity_bps_fp2(tox_fp2, tox_n);
        if (color) c += ansi::reset;
        if (tox_n > 0)
        {
            char tn[24];
            std::snprintf(tn, sizeof(tn), " (n=%u)", tox_n);
            if (color) c += ansi::dim;
            c += tn;
            if (color) c += ansi::reset;
        }
        c = pad_right(c, 34);
        if (color) c += ansi::fg_cyan;
        c += "Win rate ";
        if (color) c += ansi::reset;
        if (trades > 0)
        {
            char wr[16];
            std::snprintf(wr, sizeof(wr), "%u%%", wr_bps / 100);
            if (color)
                c += wr_bps >= 5500 ? ansi::fg_br_green
                   : wr_bps >= 4500 ? ansi::fg_gray
                   : ansi::fg_br_yel;
            c += wr;
            if (color) c += ansi::reset;
        }
        else
        {
            c += "-";
        }
        rows.push_back(row(c, color));
    }

    // Queue pos: 0 = all at front, 10000 = all at back. Populated by
    // QueueAwareBookAdapter; other adapters return 0 -> dashes.
    {
        std::string c;
        if (color) c += ansi::fg_cyan;
        c += "Quotes        ";
        if (color) c += ansi::reset;
        if (live_qs > 0)
        {
            char qb[24];
            std::snprintf(qb, sizeof(qb), "%u live", live_qs);
            c += qb;
        }
        else
        {
            c += "-";
        }
        c = pad_right(c, 34);
        if (color) c += ansi::fg_cyan;
        c += "Queue pos ";
        if (color) c += ansi::reset;
        if (live_qs > 0)
        {
            char qp[16];
            std::snprintf(qp, sizeof(qp), "%u%%", qpos_bps / 100);
            c += qp;
        }
        else
        {
            c += "-";
        }
        rows.push_back(row(c, color));
    }

    {
        std::string c;
        if (color) c += ansi::fg_cyan;
        c += "Risk    ";
        if (color) c += ansi::reset;
        c += "halt:";
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
        if (cfg_.risk_max_position_value > 0.0 && last_px > 0 && pos_qty_fp8 != 0)
        {
            const double qty       = static_cast<double>(pos_qty_fp8) / 1.0e8;
            const double last      = static_cast<double>(last_px)     / 1.0e8;
            const double inv_value = std::abs(qty) * last;
            const double pct       = inv_value / cfg_.risk_max_position_value * 100.0;
            char buf3[48];
            std::snprintf(buf3, sizeof(buf3), "  inv %.0f%%", pct);
            c += buf3;
        }
        rows.push_back(row(c, color));
    }

    {
        const auto log_d   = stats_.ring_drops_logging.load(std::memory_order_relaxed);
        const auto risk_d  = stats_.ring_drops_risk.load(std::memory_order_relaxed);
        const auto stats_d = stats_.ring_drops_stats.load(std::memory_order_relaxed);
        const auto mm_d    = stats_.ring_drops_mm.load(std::memory_order_relaxed);
        auto emit = [&](std::string& c, const char* lbl, std::uint64_t v) {
            c += lbl;
            if (color) c += (v > 0 ? ansi::fg_br_red : ansi::fg_gray);
            c += fmt_u64(v);
            if (color) c += ansi::reset;
        };
        std::string c;
        if (color) c += ansi::fg_cyan;
        c += "Rings   ";
        if (color) c += ansi::reset;
        emit(c, "log:",    log_d);
        emit(c, "  risk:", risk_d);
        emit(c, "  stats:",stats_d);
        emit(c, "  mm:",   mm_d);
        rows.push_back(row(c, color));
    }

    rows.push_back(row_recent_header_);

    // Seqlock reads: verify slot.seq == idx*2+2 before and after the copy;
    // a racing next-cycle writer flips seq and we render the row blank.
    constexpr int recent_rows = 4;
    event_entry snapshot[recent_rows];
    bool        have[recent_rows] = {false, false, false, false};
    {
        const std::uint64_t head = recent_head_.load(std::memory_order_acquire);
        const int start = static_cast<int>(
            (std::min)(static_cast<std::uint64_t>(recent_rows), head));
        for (int i = start; i > 0; --i)
        {
            const std::uint64_t idx = head - static_cast<std::uint64_t>(i);
            const std::uint64_t expected = idx * 2 + 2;
            auto& slot = recent_[idx % recent_cap];

            const std::uint64_t s1 = slot.seq.load(std::memory_order_acquire);
            if (s1 != expected) continue;
            event_entry tmp = slot.entry;
            std::atomic_thread_fence(std::memory_order_acquire);
            const std::uint64_t s2 = slot.seq.load(std::memory_order_relaxed);
            if (s2 != expected) continue;

            const int out = recent_rows - i;
            snapshot[out] = tmp;
            have[out] = true;
        }
    }
    for (int i = 0; i < recent_rows; ++i)
    {
        std::string c;
        if (have[i])
        {
            c += clock_hhmmss(snapshot[i].ts);
            c += "  ";
            if (color) c += severity_color(snapshot[i].sev, true);
            c += severity_label(snapshot[i].sev);
            if (color) c += ansi::reset;
            c += "  ";
            c.append(snapshot[i].msg, snapshot[i].msg_len);
        }
        rows.push_back(row(c, color));
    }

    rows.push_back(row_bottom_);

    const int row_count = static_cast<int>(rows.size());

    // First frame or row-count drift: full repaint and seed the cache.
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

    // Steady state: hop to the top of the box and per-row diff. Unchanged
    // rows only emit ESC[B+CR - zero bytes of content, no flicker.
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

    // Halt banner: prepended to the status line so an operator skimming
    // a non-TTY pipe (logs, ssh session in raw mode) cannot miss it.
    // Rate-limited to 1 Hz; bell rings once on the rising edge.
    if (stats_.halt_flag.load(std::memory_order_acquire))
    {
        const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const bool due = (now_ms - plain_halt_banner_last_ms_) >= 1000;
        if (due)
        {
            std::string reason = shutdown_reason();
            if (reason.empty()) reason = "halt";

            if (!plain_halt_bell_fired_)
            {
                buf += ansi::bell;
                plain_halt_bell_fired_ = true;
            }
            buf += ansi::alarm_on;
            buf += "  HALT - ";
            buf += reason;
            buf += "  ";
            buf += ansi::reset;
            buf += '\n';
            plain_halt_banner_last_ms_ = now_ms;
        }
    }

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

}
