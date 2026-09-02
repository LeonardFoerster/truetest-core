#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <string_view>

namespace tt::quantity_scale {

inline constexpr std::uint64_t canonical_atoms = 100'000'000ULL;

// Parse a venue decimal quantity into the canonical 1e-8 atom scale without
// passing through binary floating point. The accepted wire grammar is
// deliberately narrower than a C++ number: unsigned base-10 fixed point only.
// Digits beyond the canonical precision are accepted only when they are zero,
// so representable values such as "1.000000000" remain valid while sub-atom
// quantities fail closed instead of being rounded into an economic amount.
[[nodiscard]] inline std::optional<std::int64_t>
parse_decimal_canonical_atoms(std::string_view token) noexcept
{
    if (token.empty())
        return std::nullopt;

    constexpr std::uint64_t max_atoms =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    constexpr std::uint64_t max_whole = max_atoms / canonical_atoms;

    std::size_t pos = 0;
    std::uint64_t whole = 0;
    bool saw_integer_digit = false;
    while (pos < token.size() && token[pos] >= '0' && token[pos] <= '9')
    {
        saw_integer_digit = true;
        const auto digit = static_cast<std::uint64_t>(token[pos] - '0');
        if (whole > (max_whole - digit) / 10)
            return std::nullopt;
        whole = whole * 10 + digit;
        ++pos;
    }
    if (!saw_integer_digit)
        return std::nullopt;

    std::uint64_t fraction = 0;
    std::size_t fraction_digits = 0;
    if (pos < token.size() && token[pos] == '.')
    {
        ++pos;
        const std::size_t fraction_start = pos;
        while (pos < token.size() && token[pos] >= '0' && token[pos] <= '9')
        {
            const auto digit = static_cast<std::uint64_t>(token[pos] - '0');
            if (fraction_digits < 8)
                fraction = fraction * 10 + digit;
            else if (digit != 0)
                return std::nullopt;
            ++fraction_digits;
            ++pos;
        }
        if (pos == fraction_start)
            return std::nullopt;
    }
    if (pos != token.size())
        return std::nullopt;

    while (fraction_digits < 8)
    {
        fraction *= 10;
        ++fraction_digits;
    }

    const std::uint64_t whole_atoms = whole * canonical_atoms;
    if (fraction > max_atoms - whole_atoms)
        return std::nullopt;
    return static_cast<std::int64_t>(whole_atoms + fraction);
}

inline bool from_base_nonnegative(double value,
                                  std::uint64_t scale,
                                  std::int64_t& out) noexcept
{
    out = 0;
    if (!std::isfinite(value) || value < 0.0 || scale == 0)
        return false;
    const long double scaled = static_cast<long double>(value)
        * static_cast<long double>(scale);
    // INT64_MAX rounds to 2^63 when long double has only binary64 precision.
    // Use the exact exclusive power-of-two bound and validate the rounded
    // value before conversion, so no float-to-integer overflow is possible.
    constexpr long double int64_exclusive_upper_bound = 0x1p63L;
    if (scaled < 0.0L || scaled >= int64_exclusive_upper_bound)
        return false;
    const long double rounded = std::floor(scaled + 0.5L);
    if (rounded >= int64_exclusive_upper_bound)
        return false;
    out = static_cast<std::int64_t>(rounded);
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

inline bool rescale_nonnegative_exact(std::int64_t raw,
                                      std::uint64_t source_scale,
                                      std::uint64_t target_scale,
                                      std::uint64_t& out) noexcept
{
    out = 0;
    if (raw < 0 || source_scale == 0 || target_scale == 0)
        return false;
    if (raw == 0)
        return true;

    // Production venue data and the configured book normally share the
    // canonical scale. Keep that hot path integer-only and exact.
    if (target_scale == source_scale)
    {
        out = static_cast<std::uint64_t>(raw);
        return true;
    }

    // Reduce before multiplying so the conversion is exact and cannot
    // overflow an intermediate. A positive source amount that is below one
    // target atom is not a deletion: it is unrepresentable and must fail.
    std::uint64_t numerator = static_cast<std::uint64_t>(raw);
    std::uint64_t denominator = source_scale;
    const auto raw_gcd = std::gcd(numerator, denominator);
    numerator /= raw_gcd;
    denominator /= raw_gcd;
    std::uint64_t target_reduced = target_scale;
    const auto scale_gcd = std::gcd(target_reduced, denominator);
    target_reduced /= scale_gcd;
    denominator /= scale_gcd;
    if (denominator != 1
        || (target_reduced != 0
            && numerator > std::numeric_limits<std::uint64_t>::max()
                / target_reduced))
        return false;
    out = numerator * target_reduced;
    return true;
}

inline bool rescale_nonnegative(std::int64_t raw,
                                std::uint64_t source_scale,
                                double target_scale,
                                std::uint64_t& out) noexcept
{
    out = 0;
    if (!std::isfinite(target_scale) || !(target_scale > 0.0))
        return false;

    const long double target_value = static_cast<long double>(target_scale);
    // UINT64_MAX rounds to 2^64 when long double has only double precision.
    // Compare against the exactly representable exclusive power-of-two bound
    // before converting, otherwise the cast itself can be undefined.
    constexpr long double uint64_exclusive_upper_bound = 0x1p64L;
    if (std::floor(target_value) != target_value
        || target_value >= uint64_exclusive_upper_bound)
        return false;
    return rescale_nonnegative_exact(
        raw, source_scale, static_cast<std::uint64_t>(target_value), out);
}

} // namespace tt::quantity_scale
