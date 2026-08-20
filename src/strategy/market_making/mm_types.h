#pragma once

#include "core/event.h"              // order_side
#include "execution/instrument.h"    // instrument_spec (venue source of truth)
#include "types/price.h"
#include "types/quantity_scale.h"

#include <cstddef>
#include <cstdint>
#include <limits>

// Inventory-aware market-making strategy — shared value types.
//
// SCOPE BOUNDARY (R1/A02): nothing in this module produces counterparty
// liquidity. `src/market_maker/` seeds a synthetic book for bar-mode
// backtests; that is simulation furniture. This module is a *strategy*: it
// consumes canonical market state plus authoritative inventory and emits
// quote intents that then pass pre-trade risk like any other order intent.
// The two must never be merged, and neither includes the other.
namespace truetest::mm
{

using timestamp_ns = std::int64_t;

// Signed base-asset quantity in the repository's canonical atom scale.
// Signed because inventory is signed; the orderbook's `quantity` alias is
// std::uint64_t and cannot express a short. This is a view on the existing
// scale, not a second quantity system.
using qty_atoms = std::int64_t;

// Every inventory calculation is signed. Keep admissible values well inside
// the representable range so a conservative headroom calculation can add or
// subtract two validated values without relying on signed overflow.
inline constexpr qty_atoms max_safe_inventory_atoms =
    std::numeric_limits<qty_atoms>::max() / 8;

inline constexpr qty_atoms atoms_per_unit =
    static_cast<qty_atoms>(tt::quantity_scale::canonical_atoms);

// Basis points (1 bp = 1e-4). Kept as double because the venue-derived risk
// inputs (volatility, toxicity, latency) already arrive as doubles from the
// market-state layer.
using basis_points = double;

// Compile-time ladder bound. Config `levels` is validated against it, so the
// decision payload is a fixed-size value type.
inline constexpr unsigned max_quote_levels = 8;
inline constexpr std::size_t max_quote_intents =
    static_cast<std::size_t>(max_quote_levels) * 2;
inline constexpr std::size_t max_quote_reasons = 10;

enum class mm_state : std::uint8_t
{
    // Quoting both sides subject to inventory skew.
    active,
    // Hard inventory limit reached: only the inventory-reducing side quotes.
    reducing_only,
    // Fail-closed: no new intents at all, and resting quotes must be pulled.
    paused
};

enum class quote_reason : std::uint8_t
{
    normal,
    inventory_soft_limit,
    inventory_reducing_bias,
    inventory_hard_limit,
    stale_market_data,
    sequence_gap,
    unknown_inventory,
    insufficient_edge,
    invalid_market_state,
    invalid_instrument_metadata,
    post_only_cross_prevented,
    // Quote churn guard (quotes.minimum_refresh_ticks /
    // quotes.minimum_quote_lifetime_ms): resting quotes stay as they are.
    quote_refresh_throttled,
    // Strategy asked to evaluate before configure() succeeded.
    not_configured
};

[[nodiscard]] const char* to_string(mm_state state) noexcept;
[[nodiscard]] const char* to_string(quote_reason reason) noexcept;

// Validated fixed-point projection of the venue's instrument_spec onto the
// grid the strategy quotes on. instrument_spec stays the source of truth;
// this is the exactness check plus unit conversion, done once at startup so
// evaluate() never re-derives it.
struct mm_instrument
{
    std::int64_t tick_raw = 0;      // price increment in Price::raw() units
    qty_atoms    lot_atoms = 0;     // quantity increment in canonical atoms
    qty_atoms    min_qty_atoms = 0; // venue minimum order size, 0 = unset
    basis_points maker_fee_bps = 0.0;
};

enum class instrument_status : std::uint8_t
{
    ok,
    invalid_tick,
    invalid_lot,
    invalid_min_qty,
    invalid_fee
};

// Converts a venue instrument_spec into the strategy's integer grid.
// Refuses to approximate: a tick/lot size that is not exactly representable
// on the Price/atom grid is a configuration error, not something to round.
[[nodiscard]] instrument_status make_mm_instrument(const instrument_spec& spec,
                                                   mm_instrument& out) noexcept;

[[nodiscard]] const char* describe(instrument_status status) noexcept;

// ── integer grid helpers ────────────────────────────────────────────────────

// Largest multiple of `lot` not exceeding `atoms`. Negative input clamps to
// zero: a negative order size is never emitted.
[[nodiscard]] constexpr qty_atoms lot_floor(qty_atoms atoms, qty_atoms lot) noexcept
{
    if (atoms <= 0 || lot <= 0)
        return 0;
    return (atoms / lot) * lot;
}

// Maker-safe price rounding on the integer tick grid. floor for bids and
// ceil for asks so rounding can never make a quote more aggressive than the
// computed target.
[[nodiscard]] constexpr std::int64_t tick_floor(std::int64_t raw,
                                                std::int64_t tick) noexcept
{
    if (tick <= 0)
        return raw;
    const std::int64_t q = raw / tick;
    const std::int64_t r = raw % tick;
    return (r < 0 ? q - 1 : q) * tick;
}

[[nodiscard]] constexpr std::int64_t tick_ceil(std::int64_t raw,
                                               std::int64_t tick) noexcept
{
    if (tick <= 0)
        return raw;
    const std::int64_t q = raw / tick;
    const std::int64_t r = raw % tick;
    return (r > 0 ? q + 1 : q) * tick;
}

[[nodiscard]] constexpr bool is_tick_valid(Price p, std::int64_t tick) noexcept
{
    return tick > 0 && (p.raw() % tick) == 0;
}

[[nodiscard]] constexpr bool is_lot_valid(qty_atoms atoms, qty_atoms lot) noexcept
{
    return lot > 0 && atoms >= 0 && (atoms % lot) == 0;
}

} // namespace truetest::mm
