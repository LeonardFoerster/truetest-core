#include "mm_types.h"

#include <cmath>

namespace truetest::mm
{

namespace
{

// A venue increment must land exactly on the fixed-point grid. The tolerance
// only absorbs the binary representation error of a decimal literal
// (0.1 * 10000 == 1000.0000000000001), never a genuinely finer grid.
constexpr double grid_epsilon = 1e-6;

bool exact_on_grid(double value, double scale, std::int64_t& out) noexcept
{
    if (!std::isfinite(value) || value <= 0.0 || !std::isfinite(scale) || scale <= 0.0)
        return false;
    const double scaled = value * scale;
    if (!std::isfinite(scaled) || scaled < 1.0 || scaled > 9.0e18)
        return false;
    const double rounded = std::nearbyint(scaled);
    if (std::fabs(scaled - rounded) > grid_epsilon * std::fmax(1.0, std::fabs(scaled)))
        return false;
    out = static_cast<std::int64_t>(rounded);
    return out > 0;
}

} // namespace

const char* to_string(mm_state state) noexcept
{
    switch (state)
    {
    case mm_state::active:        return "ACTIVE";
    case mm_state::reducing_only: return "REDUCING_ONLY";
    case mm_state::paused:        return "PAUSED";
    }
    return "UNKNOWN";
}

const char* to_string(quote_reason reason) noexcept
{
    switch (reason)
    {
    case quote_reason::normal:                     return "NORMAL";
    case quote_reason::inventory_soft_limit:       return "INVENTORY_SOFT_LIMIT";
    case quote_reason::inventory_reducing_bias:    return "INVENTORY_REDUCING_BIAS";
    case quote_reason::inventory_hard_limit:       return "INVENTORY_HARD_LIMIT";
    case quote_reason::stale_market_data:          return "STALE_MARKET_DATA";
    case quote_reason::sequence_gap:               return "SEQUENCE_GAP";
    case quote_reason::unknown_inventory:          return "UNKNOWN_INVENTORY";
    case quote_reason::insufficient_edge:          return "INSUFFICIENT_EDGE";
    case quote_reason::invalid_market_state:       return "INVALID_MARKET_STATE";
    case quote_reason::invalid_instrument_metadata:return "INVALID_INSTRUMENT_METADATA";
    case quote_reason::post_only_cross_prevented:  return "POST_ONLY_CROSS_PREVENTED";
    case quote_reason::quote_refresh_throttled:    return "QUOTE_REFRESH_THROTTLED";
    case quote_reason::not_configured:             return "NOT_CONFIGURED";
    }
    return "UNKNOWN";
}

const char* describe(instrument_status status) noexcept
{
    switch (status)
    {
    case instrument_status::ok:
        return "ok";
    case instrument_status::invalid_tick:
        return "instrument.tick_size missing, non-positive, or finer than the "
               "Price fixed-point grid (1e-4)";
    case instrument_status::invalid_lot:
        return "instrument.lot_size missing, non-positive, or finer than the "
               "canonical quantity atom grid (1e-8)";
    case instrument_status::invalid_min_qty:
        return "instrument.min_qty is negative or not representable in "
               "canonical quantity atoms";
    case instrument_status::invalid_fee:
        return "instrument.maker_rate is not finite or negative";
    }
    return "unknown";
}

instrument_status make_mm_instrument(const instrument_spec& spec,
                                     mm_instrument& out) noexcept
{
    out = mm_instrument{};

    std::int64_t tick_raw = 0;
    if (!exact_on_grid(spec.tick_size, static_cast<double>(Price::SCALE), tick_raw))
        return instrument_status::invalid_tick;

    std::int64_t lot_atoms = 0;
    if (!exact_on_grid(spec.lot_size, static_cast<double>(atoms_per_unit), lot_atoms))
        return instrument_status::invalid_lot;

    std::int64_t min_qty_atoms = 0;
    if (spec.min_qty != 0.0)
    {
        if (!exact_on_grid(spec.min_qty, static_cast<double>(atoms_per_unit),
                           min_qty_atoms))
            return instrument_status::invalid_min_qty;
    }

    if (!std::isfinite(spec.maker_rate) || spec.maker_rate < 0.0)
        return instrument_status::invalid_fee;

    out.tick_raw = tick_raw;
    out.lot_atoms = lot_atoms;
    out.min_qty_atoms = min_qty_atoms;
    // instrument_spec carries fee as a rate fraction (0.001 == 10 bps).
    out.maker_fee_bps = spec.maker_rate * 10000.0;
    return instrument_status::ok;
}

} // namespace truetest::mm
