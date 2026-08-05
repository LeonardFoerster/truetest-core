#pragma once

// Presentation-scale helpers for the ImGui desk.
// Engine conventions stay native in dashboard_snapshot; this only formats.
//
//   position qty   — signed (negative = short)
//   win_rate       — percent 0..100 in several snapshot fields
//   drawdown       — positive percent from peak
//   side chars     — 'L'/'S' position, 'B'/'S' order/fill

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace truetest::ui::desk {

inline double abs_qty(double qty)
{
    return qty < 0.0 ? -qty : qty;
}

inline const char* position_side(double qty)
{
    if (qty > 0.0) return "LONG";
    if (qty < 0.0) return "SHORT";
    return "FLAT";
}

inline const char* side_word(char side)
{
    switch (side)
    {
    case 'L': case 'l': return "LONG";
    case 'S': case 's': return "SELL";
    case 'B': case 'b': return "BUY";
    default:            return "?";
    }
}

inline bool side_is_buy_or_long(char side)
{
    return side == 'L' || side == 'l' || side == 'B' || side == 'b';
}

inline std::string fmt_num(double v, int decimals = 2)
{
    char buf[64];
    if (!std::isfinite(v))
        return "—";
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

inline std::string fmt_usd(double v, int decimals = 2)
{
    char buf[72];
    if (!std::isfinite(v))
        return "—";
    // Compact large notionals
    const double a = std::fabs(v);
    if (a >= 1e6)
        std::snprintf(buf, sizeof(buf), "%.*fM", decimals > 0 ? 2 : 0, v / 1e6);
    else if (a >= 1e4)
        std::snprintf(buf, sizeof(buf), "%.*fK", decimals > 0 ? 1 : 0, v / 1e3);
    else
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

inline std::string fmt_full_usd(double v, int decimals = 2)
{
    char buf[64];
    if (!std::isfinite(v))
        return "—";
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

inline std::string fmt_signed_usd(double v, int decimals = 2)
{
    char buf[64];
    if (!std::isfinite(v))
        return "—";
    std::snprintf(buf, sizeof(buf), "%+.*f", decimals, v);
    return buf;
}

inline std::string fmt_pct(double value, bool already_percent = false)
{
    const double pct = already_percent ? value : value * 100.0;
    char buf[32];
    if (!std::isfinite(pct))
        return "—";
    std::snprintf(buf, sizeof(buf), "%+.2f%%", pct);
    return buf;
}

inline std::string fmt_pct_abs(double value, bool already_percent = false)
{
    const double pct = already_percent ? value : value * 100.0;
    char buf[32];
    if (!std::isfinite(pct))
        return "—";
    std::snprintf(buf, sizeof(buf), "%.2f%%", pct);
    return buf;
}

inline std::string fmt_bps(double bps)
{
    char buf[32];
    if (!std::isfinite(bps))
        return "—";
    std::snprintf(buf, sizeof(buf), "%.1f bps", bps);
    return buf;
}

inline std::string fmt_px(double px)
{
    if (!std::isfinite(px) || px <= 0.0)
        return "—";
    if (px >= 1000.0)
        return fmt_num(px, 1);
    if (px >= 1.0)
        return fmt_num(px, 2);
    return fmt_num(px, 4);
}

inline std::string fmt_qty(double q)
{
    if (!std::isfinite(q))
        return "—";
    const double a = std::fabs(q);
    if (a >= 100.0)
        return fmt_num(q, 2);
    if (a >= 1.0)
        return fmt_num(q, 4);
    return fmt_num(q, 6);
}

inline std::string fmt_age(std::int64_t seconds)
{
    char buf[32];
    if (seconds < 0)
        return "—";
    if (seconds < 60)
        std::snprintf(buf, sizeof(buf), "%llds", static_cast<long long>(seconds));
    else if (seconds < 3600)
        std::snprintf(buf, sizeof(buf), "%lldm", static_cast<long long>(seconds / 60));
    else
        std::snprintf(buf, sizeof(buf), "%lldh%lldm",
                      static_cast<long long>(seconds / 3600),
                      static_cast<long long>((seconds % 3600) / 60));
    return buf;
}

inline int px_decimals(double px)
{
    if (px >= 1000.0) return 1;
    if (px >= 1.0) return 2;
    return 4;
}

} // namespace truetest::ui::desk
