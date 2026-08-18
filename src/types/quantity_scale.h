#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace tt::quantity_scale {

inline constexpr std::uint64_t canonical_atoms = 100'000'000ULL;

inline bool from_base_nonnegative(double value,
                                  std::uint64_t scale,
                                  std::int64_t& out) noexcept
{
    out = 0;
    if (!std::isfinite(value) || value < 0.0 || scale == 0)
        return false;
    const long double scaled = static_cast<long double>(value)
        * static_cast<long double>(scale);
    if (scaled < 0.0L
        || scaled > static_cast<long double>(
            std::numeric_limits<std::int64_t>::max()))
        return false;
    out = static_cast<std::int64_t>(std::llround(scaled));
    return true;
}

// Market-data records carry an integer plus an explicit scale. Execution
// adapters consume base-unit doubles; orderbooks consume configured atoms.
inline double to_base(std::int64_t raw, std::uint64_t scale) noexcept
{
    if (raw <= 0 || scale == 0)
        return 0.0;
    const double value = static_cast<double>(raw) / static_cast<double>(scale);
    return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

inline bool rescale_nonnegative(std::int64_t raw,
                                std::uint64_t source_scale,
                                double target_scale,
                                std::uint64_t& out) noexcept
{
    out = 0;
    if (raw == 0)
        return source_scale != 0 && std::isfinite(target_scale)
            && target_scale > 0.0;
    if (raw < 0 || source_scale == 0 || !std::isfinite(target_scale)
        || !(target_scale > 0.0))
        return false;

    // Production venue data and the configured book normally share the
    // canonical scale. Keep that hot path integer-only and exact.
    if (static_cast<long double>(target_scale)
        == static_cast<long double>(source_scale))
    {
        out = static_cast<std::uint64_t>(raw);
        return true;
    }

    const long double scaled = static_cast<long double>(raw)
        * static_cast<long double>(target_scale)
        / static_cast<long double>(source_scale);
    if (!(scaled >= 0.0L)
        || scaled > static_cast<long double>(
            std::numeric_limits<long long>::max()))
        return false;
    out = static_cast<std::uint64_t>(std::llround(scaled));
    return true;
}

} // namespace tt::quantity_scale
