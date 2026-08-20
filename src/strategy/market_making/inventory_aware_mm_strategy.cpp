#include "inventory_aware_mm_strategy.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace truetest::mm
{

namespace
{

constexpr double bps_scale = 10000.0;

[[nodiscard]] bool inventory_value_sane(qty_atoms v) noexcept
{
    return v >= -max_safe_inventory_atoms && v <= max_safe_inventory_atoms;
}

[[nodiscard]] std::int64_t tick_distance_saturating(std::int64_t lhs,
                                                      std::int64_t rhs,
                                                      std::int64_t tick) noexcept
{
    if (tick <= 0)
        return std::numeric_limits<std::int64_t>::max();

    // Conversion to uint64_t is modulo 2^64, so either subtraction exactly
    // represents the magnitude even for (INT64_MIN, INT64_MAX).
    const std::uint64_t distance = lhs >= rhs
        ? static_cast<std::uint64_t>(lhs) - static_cast<std::uint64_t>(rhs)
        : static_cast<std::uint64_t>(rhs) - static_cast<std::uint64_t>(lhs);
    const std::uint64_t ticks = distance / static_cast<std::uint64_t>(tick);
    if (ticks > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(ticks);
}

// Both inputs are signed wall-clock-style nanoseconds. Never subtract them
// as signed values: ordered extreme timestamps are a valid arithmetic input
// and their elapsed duration may exceed INT64_MAX.
[[nodiscard]] std::int64_t elapsed_ns_saturating(std::int64_t newer,
                                                  std::int64_t older) noexcept
{
    if (newer <= older)
        return 0;
    const std::uint64_t elapsed = static_cast<std::uint64_t>(newer)
        - static_cast<std::uint64_t>(older);
    if (elapsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(elapsed);
}

// Quantity conversion to the nearest atom that refuses to wrap. A size that
// cannot be represented yields 0, i.e. no quote — never a wrapped one.
// Nearest, not floor: the binary representation of a multiplier such as 0.85
// lands one ulp low, and flooring there would silently drop a whole lot. The
// maker-safe granularity is enforced afterwards by lot_floor.
[[nodiscard]] qty_atoms atoms_from_double(double v) noexcept
{
    // llround has undefined behaviour for values outside its result type.
    // Check the configured safety range first; never let a transient sizing
    // product reach that conversion merely to discard it afterwards.
    if (!std::isfinite(v) || v <= 0.0
        || v > static_cast<double>(max_safe_inventory_atoms))
        return 0;
    const double f = static_cast<double>(std::llround(v));
    if (f > static_cast<double>(max_safe_inventory_atoms) || f <= 0.0)
        return 0;
    return static_cast<qty_atoms>(f);
}

[[nodiscard]] bool price_from_double_checked(double value, Price& out) noexcept
{
    // The double representation of INT64_MAX rounds to 2^63, which is one
    // beyond the signed destination. Keep one representable double below it
    // and reject the fringe rather than invoke llround out of range.
    const double max_safe_rounded = std::nextafter(
        static_cast<double>(std::numeric_limits<std::int64_t>::max()), 0.0);
    if (!std::isfinite(value) || value <= 0.0 || value > max_safe_rounded)
        return false;
    out = Price(static_cast<std::int64_t>(std::llround(value)));
    return true;
}

// Maker-safe conversion of a target price (in Price raw units, as a double)
// onto the venue tick grid. floor for bids, ceil for asks: rounding can only
// move a quote away from the touch, never through it.
[[nodiscard]] bool price_on_tick(double raw_target, std::int64_t tick, bool round_down,
                                 std::int64_t& out) noexcept
{
    if (!std::isfinite(raw_target) || tick <= 0)
        return false;
    const double tick_d = static_cast<double>(tick);
    const double q = round_down ? std::floor(raw_target / tick_d)
                                : std::ceil(raw_target / tick_d);
    if (!std::isfinite(q) || std::fabs(q) > 9.0e18 / tick_d)
        return false;
    out = static_cast<std::int64_t>(q) * tick;
    return true;
}

[[nodiscard]] bool market_inputs_valid(const market_snapshot& m,
                                       const strategy_context& ctx) noexcept
{
    if (m.best_bid.raw() <= 0 || m.best_ask.raw() <= 0)
        return false;
    // No explicit crossed/locked book state exists in the canonical market
    // layer, so a non-positive spread is rejected rather than guessed at.
    if (m.best_ask.raw() <= m.best_bid.raw())
        return false;
    if (m.best_bid_qty < 0 || m.best_ask_qty < 0)
        return false;
    // Denominator of the microprice. Zero displayed size on both sides means
    // the quoted prices are phantom, so this fails closed instead of falling
    // back to a mid derived from prices nothing backs.
    if (m.best_bid_qty == 0 && m.best_ask_qty == 0)
        return false;
    if (!inventory_value_sane(m.best_bid_qty) || !inventory_value_sane(m.best_ask_qty))
        return false;

    if (!std::isfinite(m.short_horizon_volatility_bps) || m.short_horizon_volatility_bps < 0.0)
        return false;
    if (!std::isfinite(m.toxicity_risk_bps) || m.toxicity_risk_bps < 0.0)
        return false;
    if (!std::isfinite(m.latency_risk_bps) || m.latency_risk_bps < 0.0)
        return false;
    if (!std::isfinite(m.short_flow_signal) || std::fabs(m.short_flow_signal) > 1.0)
        return false;

    // Look-ahead guard: only state whose venue event time and local receive
    // time are at or before the decision may influence a decision.
    if (m.event_time_ns > ctx.decision_time_ns)
        return false;
    if (m.receive_time_ns > ctx.decision_time_ns)
        return false;

    return true;
}

} // namespace

mm_config_status InventoryAwareMarketMakingStrategy::configure(const mm_config& cfg) noexcept
{
    const auto status = validate(cfg);
    if (!status.ok)
    {
        configured_ = false;
        return status;
    }

    // Startup-only copy; the std::string member is never touched again, so
    // evaluate() stays allocation-free.
    cfg_ = cfg;
    config_hash_ = config_hash(cfg_);
    configured_ = true;
    return status;
}

void InventoryAwareMarketMakingStrategy::publish(const quote_decision& d,
                                                 const inventory_snapshot& inv,
                                                 const strategy_context& ctx) const noexcept
{
    if (sink_ == nullptr)
        return;

    mm_decision_record rec{};
    rec.strategy_id = cfg_.strategy_id;
    rec.strategy_version = version;
    rec.config_hash = config_hash_;
    rec.decision_time_ns = d.decision_time_ns;
    rec.market_snapshot_id = d.market_snapshot_id;
    rec.symbol_id = ctx.symbol_id;
    rec.fair_value = d.fair_value;
    rec.reservation_price = d.reservation_price;
    rec.inventory = inv.signed_base_position;
    rec.inventory_utilization = d.inventory_utilization;
    rec.market_age_ns = d.market_age_ns;
    rec.half_spread_bps = d.target_half_spread_bps;
    rec.bid_size = d.bid_size;
    rec.ask_size = d.ask_size;
    rec.number_of_quote_intents = static_cast<std::uint8_t>(d.intents.size());
    rec.state = d.state;
    rec.reasons = d.reasons;
    rec.decision_hash = decision_hash(d);

    sink_->on_decision(rec);
}

strategy_result InventoryAwareMarketMakingStrategy::evaluate(
    const market_snapshot& market,
    const inventory_snapshot& inv,
    const strategy_context& ctx) noexcept
{
    strategy_result result{};
    quote_decision& d = result.decision;
    d.state = mm_state::paused;
    d.decision_time_ns = ctx.decision_time_ns;
    d.market_snapshot_id = market.snapshot_id;
    d.market_age_ns = elapsed_ns_saturating(ctx.decision_time_ns, market.receive_time_ns);
    d.requote = false;
    d.cancel_resting_quotes = true;

    if (!configured_)
    {
        result.status = strategy_status::not_configured;
        d.reasons.push_unique(quote_reason::not_configured);
        publish(d, inv, ctx);
        return result;
    }

    const mm_instrument& ins = ctx.instrument;
    if (ins.tick_raw <= 0 || ins.lot_atoms <= 0 || ins.min_qty_atoms < 0
        || !std::isfinite(ins.maker_fee_bps) || ins.maker_fee_bps < 0.0)
    {
        result.status = strategy_status::invalid_instrument;
        d.reasons.push_unique(quote_reason::invalid_instrument_metadata);
        publish(d, inv, ctx);
        return result;
    }

    result.status = strategy_status::ok;

    if (!market_inputs_valid(market, ctx))
    {
        d.reasons.push_unique(quote_reason::invalid_market_state);
        publish(d, inv, ctx);
        return result;
    }

    // ── fail-closed gates (all applicable reasons recorded, not just the
    //    first, so telemetry shows every reason a quote was withheld) ───────
    bool must_pause = false;

    if (!market.sequence_valid)
    {
        d.reasons.push_unique(quote_reason::sequence_gap);
        if (cfg_.safety.pause_on_sequence_gap)
            must_pause = true;
    }

    if (d.market_age_ns > cfg_.safety.max_market_data_age_ms * 1'000'000LL)
    {
        d.reasons.push_unique(quote_reason::stale_market_data);
        must_pause = true;
    }

    if (!inv.authoritative)
    {
        d.reasons.push_unique(quote_reason::unknown_inventory);
        if (cfg_.safety.require_authoritative_inventory)
            must_pause = true;
    }

    if (!inventory_value_sane(inv.signed_base_position)
        || !inventory_value_sane(inv.worst_case_position_if_all_buys_fill)
        || !inventory_value_sane(inv.worst_case_position_if_all_sells_fill)
        || inv.hard_limit < 0 || inv.hard_limit > max_safe_inventory_atoms)
    {
        d.reasons.push_unique(quote_reason::unknown_inventory);
        must_pause = true;
    }

    if (must_pause)
    {
        publish(d, inv, ctx);
        return result;
    }

    // ── effective hard limit: the tighter of config and ledger ─────────────
    qty_atoms hard = cfg_.inventory.hard_limit_base;
    if (inv.hard_limit > 0)
        hard = std::min(hard, inv.hard_limit);
    if (hard <= 0)
    {
        d.reasons.push_unique(quote_reason::inventory_hard_limit);
        publish(d, inv, ctx);
        return result;
    }

    // ── fair value ─────────────────────────────────────────────────────────
    const double bid_d = static_cast<double>(market.best_bid.raw());
    const double ask_d = static_cast<double>(market.best_ask.raw());
    const double bid_q = static_cast<double>(market.best_bid_qty);
    const double ask_q = static_cast<double>(market.best_ask_qty);

    const double mid = 0.5 * (bid_d + ask_d);
    const double size_sum = bid_q + ask_q;
    double microprice = mid;
    double imbalance = 0.0;
    if (size_sum > 0.0)
    {
        microprice = (ask_d * bid_q + bid_d * ask_q) / size_sum;
        imbalance = (bid_q - ask_q) / size_sum;
    }
    const double book_half_spread = 0.5 * (ask_d - bid_d);

    const double fair_d = mid
        + cfg_.fair_value.microprice_weight * (microprice - mid)
        + cfg_.fair_value.imbalance_weight * imbalance * book_half_spread
        + cfg_.fair_value.short_flow_weight * market.short_flow_signal * book_half_spread;

    if (!std::isfinite(fair_d) || fair_d <= 0.0)
    {
        d.reasons.push_unique(quote_reason::invalid_market_state);
        publish(d, inv, ctx);
        return result;
    }

    // ── inventory utilisation and reservation price ────────────────────────
    const double u = std::clamp(
        static_cast<double>(inv.signed_base_position) / static_cast<double>(hard),
        -1.0, 1.0);
    const double abs_u = std::fabs(u);

    double skew_boost = 1.0;
    if (cfg_.inventory.soft_limit_skew_boost > 0.0 && abs_u > cfg_.inventory.soft_limit_ratio)
    {
        const double span = 1.0 - cfg_.inventory.soft_limit_ratio; // > 0 by validation
        skew_boost += cfg_.inventory.soft_limit_skew_boost
            * ((abs_u - cfg_.inventory.soft_limit_ratio) / span);
    }

    // skew_bps is strictly increasing in u (u * boost(|u|) is), so the
    // reservation price is strictly decreasing in u. That monotonicity is a
    // hard invariant, not an emergent property.
    const double skew_bps =
        cfg_.inventory.reservation_skew_bps_at_hard_limit * u * skew_boost;
    const double reservation_d = fair_d * (1.0 - skew_bps / bps_scale);

    if (!std::isfinite(reservation_d) || reservation_d <= 0.0)
    {
        d.reasons.push_unique(quote_reason::invalid_market_state);
        publish(d, inv, ctx);
        return result;
    }

    if (!price_from_double_checked(fair_d, d.fair_value)
        || !price_from_double_checked(reservation_d, d.reservation_price))
    {
        d.reasons.push_unique(quote_reason::invalid_market_state);
        publish(d, inv, ctx);
        return result;
    }
    d.inventory_utilization = u;

    // ── state machine ──────────────────────────────────────────────────────
    mm_state state = mm_state::active;
    if (abs_u >= 1.0)
    {
        state = mm_state::reducing_only;
        d.reasons.push_unique(quote_reason::inventory_hard_limit);
    }
    else if (abs_u >= cfg_.inventory.reducing_bias_ratio)
    {
        d.reasons.push_unique(quote_reason::inventory_reducing_bias);
    }
    else if (abs_u > 0.0 && abs_u >= cfg_.inventory.soft_limit_ratio)
    {
        d.reasons.push_unique(quote_reason::inventory_soft_limit);
    }

    // ── spread controller ──────────────────────────────────────────────────
    const double fee_component_bps = cfg_.spread.fee_buffer_bps
        + cfg_.spread.maker_fee_multiplier * ins.maker_fee_bps;

    const double raw_half_spread_bps = fee_component_bps
        + cfg_.spread.volatility_multiplier * market.short_horizon_volatility_bps
        + cfg_.spread.toxicity_multiplier * market.toxicity_risk_bps
        + cfg_.spread.latency_buffer_bps
        + cfg_.spread.latency_multiplier * market.latency_risk_bps;

    const double half_spread_bps = std::clamp(
        std::max(cfg_.spread.min_half_spread_bps, raw_half_spread_bps),
        cfg_.spread.min_half_spread_bps,
        cfg_.spread.max_half_spread_bps);

    d.target_half_spread_bps = half_spread_bps;

    // ── size controller ────────────────────────────────────────────────────
    const double k = cfg_.inventory.size_skew_strength;
    const double bid_multiplier = std::clamp(1.0 - k * u,
                                             cfg_.inventory.min_size_multiplier,
                                             cfg_.inventory.max_size_multiplier);
    const double ask_multiplier = std::clamp(1.0 + k * u,
                                             cfg_.inventory.min_size_multiplier,
                                             cfg_.inventory.max_size_multiplier);

    const double base_d = static_cast<double>(cfg_.quotes.base_size);
    qty_atoms bid_size = lot_floor(atoms_from_double(base_d * bid_multiplier), ins.lot_atoms);
    qty_atoms ask_size = lot_floor(atoms_from_double(base_d * ask_multiplier), ins.lot_atoms);

    // Reducing band: the inventory-increasing side is cut hard but still
    // quotes, so the book keeps a two-sided presence while the position
    // works itself off. The cut also applies at and beyond the hard limit so
    // the reported side size stays monotone in |u| even where the side is
    // about to be suppressed outright.
    if (abs_u >= cfg_.inventory.reducing_bias_ratio)
    {
        const double factor = cfg_.inventory.reducing_size_factor;
        if (u > 0.0)
            bid_size = lot_floor(atoms_from_double(static_cast<double>(bid_size) * factor),
                                 ins.lot_atoms);
        else if (u < 0.0)
            ask_size = lot_floor(atoms_from_double(static_cast<double>(ask_size) * factor),
                                 ins.lot_atoms);
    }

    // ── hard-limit headroom over the worst-case order ledger ───────────────
    // The bound is on the worst case, not the current position: pending buys
    // that would breach the limit if they all filled block further buys even
    // while the realised position is still inside the band.
    const qty_atoms worst_long =
        std::max(inv.signed_base_position, inv.worst_case_position_if_all_buys_fill);
    const qty_atoms worst_short =
        std::min(inv.signed_base_position, inv.worst_case_position_if_all_sells_fill);

    qty_atoms buy_headroom = lot_floor(std::max<qty_atoms>(hard - worst_long, 0), ins.lot_atoms);
    qty_atoms sell_headroom = lot_floor(std::max<qty_atoms>(worst_short + hard, 0), ins.lot_atoms);

    const bool allow_buy = buy_headroom > 0;
    const bool allow_sell = sell_headroom > 0;

    if (!allow_buy || !allow_sell)
    {
        d.reasons.push_unique(quote_reason::inventory_hard_limit);
        state = mm_state::reducing_only;
    }

    // Reported side sizes are what the strategy actually intends to show, so
    // a side the hard limit has closed reports zero rather than the size it
    // would have quoted.
    d.bid_size = allow_buy ? bid_size : 0;
    d.ask_size = allow_sell ? ask_size : 0;

    d.state = state;
    d.cancel_resting_quotes = false;
    d.requote = true;

    // ── edge check ─────────────────────────────────────────────────────────
    // The configured cap can clip the half spread below what fees alone need.
    // Quoting there is a structural loss, so nothing is emitted.
    if (half_spread_bps < fee_component_bps)
    {
        d.reasons.push_unique(quote_reason::insufficient_edge);
        publish(d, inv, ctx);
        return result;
    }

    // ── quote ladder ───────────────────────────────────────────────────────
    std::int64_t prev_bid_raw = 0;
    std::int64_t prev_ask_raw = 0;
    bool have_prev = false;
    bool cross_prevented = false;

    for (unsigned level = 0; level < cfg_.levels; ++level)
    {
        const double offset_bps =
            half_spread_bps + static_cast<double>(level) * cfg_.quotes.level_spacing_bps;

        std::int64_t bid_raw = 0;
        std::int64_t ask_raw = 0;
        if (!price_on_tick(reservation_d * (1.0 - offset_bps / bps_scale),
                           ins.tick_raw, /*round_down=*/true, bid_raw)
            || !price_on_tick(reservation_d * (1.0 + offset_bps / bps_scale),
                              ins.tick_raw, /*round_down=*/false, ask_raw))
        {
            d.reasons.push_unique(quote_reason::invalid_market_state);
            break;
        }

        // Strictly monotone ladder: tick rounding must never collapse two
        // levels onto the same price.
        if (have_prev)
        {
            bid_raw = std::min(bid_raw, prev_bid_raw - ins.tick_raw);
            ask_raw = std::max(ask_raw, prev_ask_raw + ins.tick_raw);
        }
        prev_bid_raw = bid_raw;
        prev_ask_raw = ask_raw;
        have_prev = true;

        // Own-book sanity: after maker-safe rounding the two sides must still
        // straddle. If they do not, there is no edge to quote at any level.
        if (level == 0 && bid_raw >= ask_raw)
        {
            d.intents.clear();
            d.reasons.push_unique(quote_reason::insufficient_edge);
            break;
        }

        if (allow_buy && bid_size > 0 && bid_raw > 0 && buy_headroom > 0)
        {
            if (cfg_.quotes.post_only && bid_raw >= market.best_ask.raw())
            {
                cross_prevented = true;
            }
            else
            {
                const qty_atoms q =
                    lot_floor(std::min(bid_size, buy_headroom), ins.lot_atoms);
                if (q > 0 && (ins.min_qty_atoms == 0 || q >= ins.min_qty_atoms))
                {
                    d.intents.push_back(quote_intent{order_side::buy, Price(bid_raw), q,
                                                     static_cast<std::uint16_t>(level),
                                                     cfg_.quotes.post_only});
                    buy_headroom -= q;
                }
            }
        }

        if (allow_sell && ask_size > 0 && sell_headroom > 0)
        {
            if (cfg_.quotes.post_only && ask_raw <= market.best_bid.raw())
            {
                cross_prevented = true;
            }
            else
            {
                const qty_atoms q =
                    lot_floor(std::min(ask_size, sell_headroom), ins.lot_atoms);
                if (q > 0 && (ins.min_qty_atoms == 0 || q >= ins.min_qty_atoms))
                {
                    d.intents.push_back(quote_intent{order_side::sell, Price(ask_raw), q,
                                                     static_cast<std::uint16_t>(level),
                                                     cfg_.quotes.post_only});
                    sell_headroom -= q;
                }
            }
        }
    }

    if (cross_prevented)
        d.reasons.push_unique(quote_reason::post_only_cross_prevented);

    // ── quote churn guard ──────────────────────────────────────────────────
    // Applied only when the decision is a plain two-sided refresh. A decision
    // that removes a side, pauses, or trips the hard limit is never throttled.
    if (d.state == mm_state::active && allow_buy && allow_sell && ctx.resting.has_quotes
        && (cfg_.quotes.minimum_refresh_ticks > 0 || cfg_.quotes.minimum_quote_lifetime_ms > 0))
    {
        const quote_intent* level0_bid = nullptr;
        const quote_intent* level0_ask = nullptr;
        for (std::size_t i = 0; i < d.intents.size(); ++i)
        {
            const quote_intent& q = d.intents[i];
            if (q.level != 0)
                continue;
            if (q.side == order_side::buy)
                level0_bid = &q;
            else
                level0_ask = &q;
        }

        if (level0_bid != nullptr && level0_ask != nullptr)
        {
            const std::int64_t age_ms = elapsed_ns_saturating(
                ctx.decision_time_ns, ctx.resting.placed_time_ns) / 1'000'000LL;
            const std::int64_t bid_move = tick_distance_saturating(
                level0_bid->price.raw(), ctx.resting.bid_price.raw(), ins.tick_raw);
            const std::int64_t ask_move = tick_distance_saturating(
                level0_ask->price.raw(), ctx.resting.ask_price.raw(), ins.tick_raw);
            const std::int64_t move_ticks = std::max(bid_move, ask_move);

            const bool lifetime_ok = age_ms >= cfg_.quotes.minimum_quote_lifetime_ms;
            const bool move_ok =
                move_ticks >= static_cast<std::int64_t>(cfg_.quotes.minimum_refresh_ticks);

            if (!lifetime_ok || !move_ok)
            {
                d.intents.clear();
                d.requote = false;
                d.reasons.push_unique(quote_reason::quote_refresh_throttled);
            }
        }
    }

    if (d.reasons.empty())
        d.reasons.push_unique(quote_reason::normal);

    publish(d, inv, ctx);
    return result;
}

} // namespace truetest::mm
