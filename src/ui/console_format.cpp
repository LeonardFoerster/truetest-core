#include "ui/console_format.h"
#include "ui/ansi.h"

#include <charconv>
#include <cstdio>
#include <cstring>

namespace truetest::ui {

namespace {

// Content rendered into 70-col inner field, wrapped to a 74-col box.
constexpr int inner_width = 70;
constexpr int content_width = inner_width - 2;

}  // (make_hline is non-static for use by ConsoleDashboard ctor)

std::string make_hline(int n, const char* middle)
{
    std::string s;
    s.reserve(static_cast<std::size_t>(n) * 3);
    for (int i = 0; i < n; ++i)
        s += middle;
    return s;
}

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

std::string fmt_price_fp8(std::int64_t fp, int decimals)
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
    // int64 seconds can produce a 16-digit hour count plus separators.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", h, m, sec);
    return buf;
}

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

double spread_bps(double bid, double ask)
{
    if (bid <= 0.0 || ask <= 0.0) return 0.0;
    const double mid = (bid + ask) * 0.5;
    return mid > 0.0 ? (ask - bid) / mid * 1e4 : 0.0;
}

std::uint64_t total_ring_drops(const streaming_stats& s)
{
    return s.ring_drops_logging.load(std::memory_order_relaxed)
         + s.ring_drops_risk.load(std::memory_order_relaxed)
         + s.ring_drops_stats.load(std::memory_order_relaxed)
         + s.ring_drops_observer.load(std::memory_order_relaxed)
         + s.ring_drops_risk_stats.load(std::memory_order_relaxed)
         + s.ring_drops_mm.load(std::memory_order_relaxed);
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
