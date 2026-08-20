#pragma once

#include "mm_types.h"

#include <array>
#include <cstdint>
#include <string>

namespace truetest::mm
{

// Fair-value model weights.
//
// `imbalance_weight` and `short_flow_weight` default to 0: the top-of-book
// imbalance term is computable from the canonical snapshot, and the
// short-flow term is a hook for a signed order-flow signal the market-state
// layer does not publish yet. Neither is turned on by default, so v1 fair
// value is exactly mid + microprice_weight * (microprice - mid).
struct fair_value_config
{
    double microprice_weight = 0.5;
    double imbalance_weight = 0.0;
    double short_flow_weight = 0.0;
};

struct inventory_config
{
    // Absolute signed base position the strategy is allowed to carry.
    qty_atoms hard_limit_base = 0;

    // |u| bands. 0 <= soft < reducing <= 1.
    double soft_limit_ratio = 0.50;
    double reducing_bias_ratio = 0.80;

    // Reservation skew applied at |u| == 1, before the soft-band boost.
    basis_points reservation_skew_bps_at_hard_limit = 8.0;

    // Extra skew inside the soft band, ramped linearly from 0 at
    // soft_limit_ratio to this multiple at |u| == 1. 0 keeps the plain
    // linear reservation model. Monotonicity of reservation in u holds for
    // any non-negative value.
    double soft_limit_skew_boost = 0.0;

    double size_skew_strength = 0.50;
    double min_size_multiplier = 0.0;
    double max_size_multiplier = 2.0;

    // Size multiplier applied to the inventory-increasing side once
    // |u| >= reducing_bias_ratio. The band stays ACTIVE (both sides quote)
    // but the increasing side is cut hard.
    double reducing_size_factor = 0.25;
};

struct spread_config
{
    basis_points min_half_spread_bps = 1.0;
    basis_points max_half_spread_bps = 50.0;

    // Configured edge floor plus the venue's own maker fee. A round trip
    // earns 2 * half_spread and pays maker fee on both legs, so covering one
    // maker fee per side is the break-even requirement.
    basis_points fee_buffer_bps = 0.0;
    double maker_fee_multiplier = 1.0;

    double volatility_multiplier = 1.0;
    double toxicity_multiplier = 1.0;

    basis_points latency_buffer_bps = 0.0;
    double latency_multiplier = 1.0;
};

struct quotes_config
{
    qty_atoms base_size = 0;
    basis_points level_spacing_bps = 2.0;
    bool post_only = true;

    // Churn guard. Evaluated against the caller-supplied resting quote state
    // in strategy_context, so the strategy itself stays stateless.
    std::uint32_t minimum_refresh_ticks = 0;
    std::int64_t minimum_quote_lifetime_ms = 0;
};

struct safety_config
{
    std::int64_t max_market_data_age_ms = 250;
    bool pause_on_sequence_gap = true;
    bool require_authoritative_inventory = true;
};

struct mm_config
{
    std::string strategy_id = "inventory_aware_mm";
    unsigned levels = 1;

    fair_value_config fair_value{};
    inventory_config inventory{};
    spread_config spread{};
    quotes_config quotes{};
    safety_config safety{};
};

struct mm_config_status
{
    bool ok = false;
    // Static storage; safe to hold. Empty only when ok.
    const char* message = "";

    explicit operator bool() const noexcept { return ok; }
};

// Config hashes are serialized as a schema-tagged, canonical little-endian
// byte stream. Keep this visible so cross-platform regression tests can pin
// the representation rather than the host object layout.
inline constexpr std::uint32_t config_hash_schema_version = 1;

[[nodiscard]] std::array<std::uint8_t, 8> canonical_u64_le(std::uint64_t value) noexcept;

// Startup-time validation. Rejects NaN/Inf, impossible orderings, and any
// quantity that is negative where the semantics forbid it. Never called on
// the hot path.
[[nodiscard]] mm_config_status validate(const mm_config& cfg) noexcept;

// Stable, order-independent-of-compiler hash over every field that can
// change a decision. Used for run reproducibility records and golden tests.
[[nodiscard]] std::uint64_t config_hash(const mm_config& cfg) noexcept;

} // namespace truetest::mm
