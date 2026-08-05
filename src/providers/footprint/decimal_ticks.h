#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>

// footprint.md §2.1/§2.2: "Use integer tick arithmetic throughout; no
// floating-point price bucketing." This header converts a venue's raw
// decimal price/quantity STRINGS directly into exact integers - no double
// ever touches the value on this path. tick_size itself must also arrive as
// an exact decimal (venue metadata/override, never a computed double) for
// the whole chain to stay exact; see
// footprint_venue_capabilities.h::resolve_footprint_tick_size for the one
// place a double tick_size is still accepted (display/threshold use only).
namespace truetest::footprint {

struct DecimalValue
{
    std::int64_t mantissa = 0; // exact value = mantissa * 10^-scale
    int scale = 0;
};

// Parses a plain decimal string ("27045.10", "0.00010000", "100", "-3.5")
// into an exact (mantissa, scale) pair. Pure integer arithmetic on the
// digit characters - never touches a floating point type. Rejects empty,
// malformed (multiple dots, non-digit characters, sign with no digits), or
// overflowing input.
inline std::optional<DecimalValue> parse_decimal(std::string_view s) noexcept
{
    if (s.empty())
        return std::nullopt;

    std::size_t i = 0;
    bool negative = false;
    if (s[i] == '-') { negative = true; ++i; }
    else if (s[i] == '+') { ++i; }
    if (i >= s.size())
        return std::nullopt;

    std::int64_t mantissa = 0;
    int scale = 0;
    bool seen_digit = false;
    bool seen_dot = false;
    for (; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c == '.')
        {
            if (seen_dot)
                return std::nullopt;
            seen_dot = true;
            continue;
        }
        if (c < '0' || c > '9')
            return std::nullopt;
        // Overflow guard - reject absurdly long inputs rather than wrap.
        if (mantissa > (std::numeric_limits<std::int64_t>::max() - 9) / 10)
            return std::nullopt;
        mantissa = mantissa * 10 + (c - '0');
        seen_digit = true;
        if (seen_dot)
            ++scale;
    }
    if (!seen_digit)
        return std::nullopt;

    return DecimalValue{negative ? -mantissa : mantissa, scale};
}

namespace detail {
inline std::optional<std::int64_t> pow10_checked(int n) noexcept
{
    if (n < 0 || n > 18)
        return std::nullopt;
    std::int64_t v = 1;
    for (int k = 0; k < n; ++k)
        v *= 10;
    return v;
}
} // namespace detail

// Converts an exact price to integer ticks via round(price / tick_size),
// using only integer arithmetic - both operands are exact decimals, never
// doubles. Returns nullopt on a non-positive tick size or overflow.
inline std::optional<std::int64_t> decimal_to_ticks(
    const DecimalValue& price, const DecimalValue& tick_size) noexcept
{
    if (tick_size.mantissa <= 0)
        return std::nullopt;

    const int common_scale = std::max(price.scale, tick_size.scale);
    const auto price_mult = detail::pow10_checked(common_scale - price.scale);
    const auto tick_mult = detail::pow10_checked(common_scale - tick_size.scale);
    if (!price_mult || !tick_mult)
        return std::nullopt;

    const std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    if (price.mantissa != 0 && std::abs(price.mantissa) > kMax / *price_mult)
        return std::nullopt;
    if (std::abs(tick_size.mantissa) > kMax / *tick_mult)
        return std::nullopt;

    const std::int64_t price_scaled = price.mantissa * (*price_mult);
    const std::int64_t tick_scaled = tick_size.mantissa * (*tick_mult);
    if (tick_scaled <= 0)
        return std::nullopt;

    // Round-to-nearest; price_scaled may be negative (nonsensical for a
    // real trade price, but handled correctly rather than UB), tick_scaled
    // is always positive here.
    if (price_scaled >= 0)
        return (price_scaled + tick_scaled / 2) / tick_scaled;
    return -((-price_scaled + tick_scaled / 2) / tick_scaled);
}

// Converts an exact quantity to integer base-qty atoms at `atom_decimals`
// fractional digits (e.g. 8 for a satoshi-like base asset). Rounds to
// nearest when atom_decimals is coarser than the source string's own
// scale; exact (no rounding) when it is not.
inline std::optional<std::int64_t> decimal_to_atoms(
    const DecimalValue& qty, int atom_decimals) noexcept
{
    if (atom_decimals < 0 || atom_decimals > 18)
        return std::nullopt;

    const int shift = atom_decimals - qty.scale;
    const std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

    if (shift >= 0)
    {
        const auto mult = detail::pow10_checked(shift);
        if (!mult)
            return std::nullopt;
        if (qty.mantissa != 0 && std::abs(qty.mantissa) > kMax / *mult)
            return std::nullopt;
        return qty.mantissa * (*mult);
    }

    const auto div = detail::pow10_checked(-shift);
    if (!div || *div == 0)
        return std::nullopt;
    const std::int64_t half = *div / 2;
    return qty.mantissa >= 0 ? (qty.mantissa + half) / (*div)
                             : -((-qty.mantissa + half) / (*div));
}

} // namespace truetest::footprint
