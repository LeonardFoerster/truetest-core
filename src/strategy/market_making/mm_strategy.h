#pragma once

#include "mm_fixed_vector.h"
#include "mm_types.h"

#include <cstdint>
#include <string_view>

namespace truetest::mm
{

// ── strategy inputs ─────────────────────────────────────────────────────────

// Canonical top-of-book state plus the risk inputs the market-state layer
// already derives. The strategy never reads a clock, a socket, or a file:
// every time value it uses arrives here or in strategy_context.
struct market_snapshot
{
    timestamp_ns event_time_ns = 0;   // venue-stamped event time
    timestamp_ns receive_time_ns = 0; // local ingest time (staleness basis)

    Price best_bid{};
    Price best_ask{};
    qty_atoms best_bid_qty = 0;
    qty_atoms best_ask_qty = 0;

    basis_points short_horizon_volatility_bps = 0.0;
    basis_points latency_risk_bps = 0.0;
    basis_points toxicity_risk_bps = 0.0;

    // Signed short-horizon order-flow signal, normalised to [-1, +1].
    // Stays 0 until a flow feed exists; fair_value.short_flow_weight is 0 by
    // default so the term is inert rather than invented.
    double short_flow_signal = 0.0;

    bool sequence_valid = true;
    std::uint64_t snapshot_id = 0;
};

// Authoritative inventory as of decision time. The worst-case fields are the
// order-ledger projection: position if every live buy (resp. sell) filled in
// full. They are what makes the hard-limit invariant hold under pending
// orders rather than only after fills land.
struct inventory_snapshot
{
    qty_atoms signed_base_position = 0;

    // Venue/ledger-imposed limit. 0 means "unset": the config limit applies.
    // When both are set the strategy takes the tighter of the two.
    qty_atoms hard_limit = 0;

    qty_atoms worst_case_position_if_all_buys_fill = 0;
    qty_atoms worst_case_position_if_all_sells_fill = 0;

    bool authoritative = false;
};

// Quotes currently resting on the venue, owned by the execution layer. Passed
// in rather than remembered so evaluate() is a pure function of its inputs.
struct resting_quote_state
{
    bool has_quotes = false;
    Price bid_price{};
    Price ask_price{};
    timestamp_ns placed_time_ns = 0;
};

struct strategy_context
{
    timestamp_ns decision_time_ns = 0;
    mm_instrument instrument{};
    resting_quote_state resting{};
    std::uint32_t symbol_id = 0;
};

// ── strategy outputs ────────────────────────────────────────────────────────

struct quote_intent
{
    order_side side = order_side::buy;
    Price price{};
    qty_atoms quantity = 0;
    std::uint16_t level = 0;
    bool post_only = true;
};

struct quote_decision
{
    mm_state state = mm_state::paused;

    Price fair_value{};
    Price reservation_price{};
    basis_points target_half_spread_bps = 0.0;
    double inventory_utilization = 0.0;

    fixed_vector<quote_intent, max_quote_intents> intents{};
    fixed_vector<quote_reason, max_quote_reasons> reasons{};

    timestamp_ns decision_time_ns = 0;
    std::uint64_t market_snapshot_id = 0;

    // Observability extras (see docs/internal/r1-inventory-aware-market-making.md).
    std::int64_t market_age_ns = 0;
    qty_atoms bid_size = 0; // level-0 side size before hard-limit capping
    qty_atoms ask_size = 0;

    // false => leave resting quotes in place (churn guard).
    bool requote = false;
    // true => the caller must pull every resting quote before doing anything
    // else. Always true when state == paused.
    bool cancel_resting_quotes = false;
};

enum class strategy_status : std::uint8_t
{
    ok,
    not_configured,
    invalid_instrument
};

// Plain status+value result, matching the repository's existing
// `<x>_status` + result-struct idiom (e.g. resolve_footprint_tick_size).
// A non-ok status always comes with a PAUSED decision carrying no intents,
// so a caller that only looks at the decision still fails closed.
struct strategy_result
{
    strategy_status status = strategy_status::not_configured;
    quote_decision decision{};

    [[nodiscard]] bool ok() const noexcept { return status == strategy_status::ok; }
};

[[nodiscard]] const char* to_string(strategy_status status) noexcept;

// Order-sensitive, padding-free hash over every decision field a consumer can
// act on. Golden/determinism tests compare this, not the raw struct bytes.
[[nodiscard]] std::uint64_t decision_hash(const quote_decision& decision) noexcept;

// ── strategy interface ──────────────────────────────────────────────────────

class IMarketMakingStrategy
{
public:
    virtual ~IMarketMakingStrategy() = default;

    // Hot path. noexcept, allocation-free after configure(), and free of any
    // network, file, database, clock, or global mutable state access.
    [[nodiscard]] virtual strategy_result evaluate(
        const market_snapshot& market,
        const inventory_snapshot& inventory,
        const strategy_context& context) noexcept = 0;

    [[nodiscard]] virtual std::string_view strategy_id() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t strategy_version() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t strategy_config_hash() const noexcept = 0;
};

} // namespace truetest::mm
