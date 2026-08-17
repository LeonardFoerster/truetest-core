#pragma once

// Economic preflight helpers.  A finite wire value is not sufficient for a
// safe accounting transition: multiplying two finite doubles or adding a fee
// to a finite notional can still overflow.  These helpers intentionally do no
// allocation and are shared by every accounting owner before it mutates.

#include "event.h"

#include <cmath>
#include <limits>

namespace fill_validation {

inline bool finite(double value) noexcept
{
    return std::isfinite(value);
}

inline bool checked_add(double lhs, double rhs, double& out) noexcept
{
    if (!finite(lhs) || !finite(rhs)) return false;
    out = lhs + rhs;
    return finite(out);
}

inline bool checked_sub(double lhs, double rhs, double& out) noexcept
{
    if (!finite(lhs) || !finite(rhs)) return false;
    out = lhs - rhs;
    return finite(out);
}

inline bool checked_mul(double lhs, double rhs, double& out) noexcept
{
    if (!finite(lhs) || !finite(rhs)) return false;
    out = lhs * rhs;
    return finite(out);
}

inline bool checked_div(double lhs, double rhs, double& out) noexcept
{
    if (!finite(lhs) || !finite(rhs) || rhs == 0.0) return false;
    out = lhs / rhs;
    return finite(out);
}

inline bool checked_abs(double value, double& out) noexcept
{
    if (!finite(value)) return false;
    out = std::abs(value);
    return finite(out);
}

inline bool valid_fill_shape(const fill_event& fill) noexcept
{
    // Rebates are represented as negative commission by several adapters, so
    // commission is deliberately finite-only.  The rest is physical fill
    // data and must have a strictly positive quantity and price.
    return !fill.get_symbol().empty() &&
           finite(fill.get_filled_quantity()) && fill.get_filled_quantity() > 0.0 &&
           finite(fill.get_fill_price()) && fill.get_fill_price() > 0.0 &&
           finite(fill.get_commission()) &&
           finite(fill.get_remaining_qty()) && fill.get_remaining_qty() >= 0.0;
}

inline bool checked_notional(double quantity, double price, double& out) noexcept
{
    return checked_mul(quantity, price, out);
}

inline bool checked_size_increment(std::size_t current, std::size_t& out) noexcept
{
    if (current == std::numeric_limits<std::size_t>::max()) return false;
    out = current + 1;
    return true;
}

} // namespace fill_validation
