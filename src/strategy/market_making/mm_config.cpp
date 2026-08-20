#include "mm_config.h"

#include <bit>
#include <cmath>

namespace truetest::mm
{

namespace
{

bool finite_in(double v, double lo, double hi) noexcept
{
    return std::isfinite(v) && v >= lo && v <= hi;
}

bool finite_nonneg(double v) noexcept
{
    return std::isfinite(v) && v >= 0.0;
}

constexpr std::uint64_t fnv_offset = 1469598103934665603ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

constexpr char config_hash_schema[] = "truetest.mm.config.v1";

void hash_bytes(std::uint64_t& h, const void* data, std::size_t n) noexcept
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i)
    {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= fnv_prime;
    }
}

void hash_u64(std::uint64_t& h, std::uint64_t v) noexcept
{
    const auto bytes = canonical_u64_le(v);
    hash_bytes(h, bytes.data(), bytes.size());
}

void hash_i64(std::uint64_t& h, std::int64_t v) noexcept
{
    hash_u64(h, static_cast<std::uint64_t>(v));
}

// Hash the IEEE-754 bit pattern so the value, not its formatting, is what
// the hash commits to. -0.0 is normalised to +0.0 so an incidental sign
// flip in a config loader cannot change a run's config hash.
void hash_f64(std::uint64_t& h, double v) noexcept
{
    if (v == 0.0)
        v = 0.0;
    hash_u64(h, std::bit_cast<std::uint64_t>(v));
}

void hash_bool(std::uint64_t& h, bool v) noexcept
{
    const unsigned char b = v ? 1u : 0u;
    hash_bytes(h, &b, 1);
}

} // namespace

std::array<std::uint8_t, 8> canonical_u64_le(std::uint64_t value) noexcept
{
    std::array<std::uint8_t, 8> bytes{};
    for (unsigned i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xFFU);
    return bytes;
}

mm_config_status validate(const mm_config& cfg) noexcept
{
    if (cfg.strategy_id.empty())
        return {false, "strategy_id must not be empty"};

    if (cfg.levels < 1)
        return {false, "levels must be >= 1"};
    if (cfg.levels > max_quote_levels)
        return {false, "levels exceeds the compile-time maximum (max_quote_levels)"};

    // ── fair value ─────────────────────────────────────────────────────────
    if (!finite_in(cfg.fair_value.microprice_weight, 0.0, 1.0))
        return {false, "fair_value.microprice_weight must be finite in [0, 1]"};
    if (!finite_in(cfg.fair_value.imbalance_weight, -1.0, 1.0))
        return {false, "fair_value.imbalance_weight must be finite in [-1, 1]"};
    if (!finite_in(cfg.fair_value.short_flow_weight, -1.0, 1.0))
        return {false, "fair_value.short_flow_weight must be finite in [-1, 1]"};

    // ── inventory ──────────────────────────────────────────────────────────
    const auto& inv = cfg.inventory;
    if (inv.hard_limit_base <= 0)
        return {false, "inventory.hard_limit_base must be > 0"};
    if (inv.hard_limit_base > max_safe_inventory_atoms)
        return {false, "inventory.hard_limit_base exceeds safe signed arithmetic bound"};
    if (!finite_in(inv.soft_limit_ratio, 0.0, 1.0))
        return {false, "inventory.soft_limit_ratio must be finite in [0, 1]"};
    if (!finite_in(inv.reducing_bias_ratio, 0.0, 1.0))
        return {false, "inventory.reducing_bias_ratio must be finite in [0, 1]"};
    if (!(inv.soft_limit_ratio < inv.reducing_bias_ratio))
        return {false,
                "inventory: 0 <= soft_limit_ratio < reducing_bias_ratio <= 1 violated"};
    if (!finite_nonneg(inv.reservation_skew_bps_at_hard_limit))
        return {false,
                "inventory.reservation_skew_bps_at_hard_limit must be finite and >= 0"};
    if (!finite_nonneg(inv.soft_limit_skew_boost))
        return {false, "inventory.soft_limit_skew_boost must be finite and >= 0"};
    // The reservation price is fair * (1 - skew/1e4); a skew that can reach
    // 10000 bps would drive it to zero or negative at the hard limit.
    if (!(inv.reservation_skew_bps_at_hard_limit * (1.0 + inv.soft_limit_skew_boost)
          < 10000.0))
        return {false,
                "inventory.reservation_skew_bps_at_hard_limit * (1 + soft_limit_skew_boost) "
                "must be < 10000 so the reservation price stays positive"};
    if (!finite_nonneg(inv.size_skew_strength))
        return {false, "inventory.size_skew_strength must be finite and >= 0"};
    if (!finite_nonneg(inv.min_size_multiplier))
        return {false, "inventory.min_size_multiplier must be finite and >= 0"};
    if (!finite_nonneg(inv.max_size_multiplier))
        return {false, "inventory.max_size_multiplier must be finite and >= 0"};
    if (!(inv.max_size_multiplier >= inv.min_size_multiplier))
        return {false, "inventory.max_size_multiplier must be >= min_size_multiplier"};
    if (inv.max_size_multiplier > 1.0e6)
        return {false, "inventory.max_size_multiplier must be <= 1e6"};
    if (!finite_in(inv.reducing_size_factor, 0.0, 1.0))
        return {false, "inventory.reducing_size_factor must be finite in [0, 1]"};

    // ── spread ─────────────────────────────────────────────────────────────
    const auto& sp = cfg.spread;
    if (!finite_nonneg(sp.min_half_spread_bps))
        return {false, "spread.min_half_spread_bps must be finite and >= 0"};
    if (!finite_nonneg(sp.max_half_spread_bps))
        return {false, "spread.max_half_spread_bps must be finite and >= 0"};
    if (!(sp.max_half_spread_bps >= sp.min_half_spread_bps))
        return {false, "spread.max_half_spread_bps must be >= min_half_spread_bps"};
    if (!finite_nonneg(sp.fee_buffer_bps))
        return {false, "spread.fee_buffer_bps must be finite and >= 0"};
    if (!finite_nonneg(sp.maker_fee_multiplier))
        return {false, "spread.maker_fee_multiplier must be finite and >= 0"};
    if (!finite_nonneg(sp.volatility_multiplier))
        return {false, "spread.volatility_multiplier must be finite and >= 0"};
    if (!finite_nonneg(sp.toxicity_multiplier))
        return {false, "spread.toxicity_multiplier must be finite and >= 0"};
    if (!finite_nonneg(sp.latency_buffer_bps))
        return {false, "spread.latency_buffer_bps must be finite and >= 0"};
    if (!finite_nonneg(sp.latency_multiplier))
        return {false, "spread.latency_multiplier must be finite and >= 0"};

    // ── quotes ─────────────────────────────────────────────────────────────
    const auto& q = cfg.quotes;
    if (q.base_size <= 0)
        return {false, "quotes.base_size must be > 0"};
    if (q.base_size > max_safe_inventory_atoms)
        return {false, "quotes.base_size exceeds safe signed arithmetic bound"};
    if (!finite_nonneg(q.level_spacing_bps))
        return {false, "quotes.level_spacing_bps must be finite and >= 0"};
    if (cfg.levels > 1 && !(q.level_spacing_bps > 0.0))
        return {false, "quotes.level_spacing_bps must be > 0 when levels > 1"};
    if (q.minimum_quote_lifetime_ms < 0)
        return {false, "quotes.minimum_quote_lifetime_ms must be >= 0"};

    // ── safety ─────────────────────────────────────────────────────────────
    if (cfg.safety.max_market_data_age_ms <= 0)
        return {false, "safety.max_market_data_age_ms must be > 0"};
    if (cfg.safety.max_market_data_age_ms
        > std::numeric_limits<std::int64_t>::max() / 1'000'000LL)
        return {false, "safety.max_market_data_age_ms exceeds nanosecond range"};

    return {true, ""};
}

std::uint64_t config_hash(const mm_config& cfg) noexcept
{
    std::uint64_t h = fnv_offset;

    // Version and the string length make the byte stream self-delimiting and
    // let a deliberate config-schema revision produce a distinct identity.
    hash_bytes(h, config_hash_schema, sizeof(config_hash_schema) - 1);
    hash_u64(h, cfg.strategy_id.size());
    hash_bytes(h, cfg.strategy_id.data(), cfg.strategy_id.size());
    hash_u64(h, cfg.levels);

    hash_f64(h, cfg.fair_value.microprice_weight);
    hash_f64(h, cfg.fair_value.imbalance_weight);
    hash_f64(h, cfg.fair_value.short_flow_weight);

    hash_i64(h, cfg.inventory.hard_limit_base);
    hash_f64(h, cfg.inventory.soft_limit_ratio);
    hash_f64(h, cfg.inventory.reducing_bias_ratio);
    hash_f64(h, cfg.inventory.reservation_skew_bps_at_hard_limit);
    hash_f64(h, cfg.inventory.soft_limit_skew_boost);
    hash_f64(h, cfg.inventory.size_skew_strength);
    hash_f64(h, cfg.inventory.min_size_multiplier);
    hash_f64(h, cfg.inventory.max_size_multiplier);
    hash_f64(h, cfg.inventory.reducing_size_factor);

    hash_f64(h, cfg.spread.min_half_spread_bps);
    hash_f64(h, cfg.spread.max_half_spread_bps);
    hash_f64(h, cfg.spread.fee_buffer_bps);
    hash_f64(h, cfg.spread.maker_fee_multiplier);
    hash_f64(h, cfg.spread.volatility_multiplier);
    hash_f64(h, cfg.spread.toxicity_multiplier);
    hash_f64(h, cfg.spread.latency_buffer_bps);
    hash_f64(h, cfg.spread.latency_multiplier);

    hash_i64(h, cfg.quotes.base_size);
    hash_f64(h, cfg.quotes.level_spacing_bps);
    hash_bool(h, cfg.quotes.post_only);
    hash_u64(h, cfg.quotes.minimum_refresh_ticks);
    hash_i64(h, cfg.quotes.minimum_quote_lifetime_ms);

    hash_i64(h, cfg.safety.max_market_data_age_ms);
    hash_bool(h, cfg.safety.pause_on_sequence_gap);
    hash_bool(h, cfg.safety.require_authoritative_inventory);

    return h;
}

} // namespace truetest::mm
