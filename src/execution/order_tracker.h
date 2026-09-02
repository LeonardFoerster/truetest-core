#pragma once

// ============================================================
// Authoritative order ledger (risk register R3).
//
// This is the single fachlich authoritative source for order state, the set
// of open orders, remaining quantity per order, and per-symbol pending
// exposure. Analytics/performance counters (Analytics::total_orders_,
// portfolio::total_fills_, dashboard perf rows) are reporting only and must
// never feed a risk decision — see
// docs/internal/r3-authoritative-risk-accounting.md.
//
// Ownership: the engine thread is the sole writer. Workers may read exactly
// one thing concurrently, active_count_atomic(); everything else is
// engine-thread-owned, exactly as before R3.
// ============================================================

#include "../core/event.h"
#include "../types/symbol_table.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <vector>

enum class order_status
{
    pending,
    open,
    partially_filled,
    filled,
    cancelled,
    rejected,
    // R3: appended so the existing ordinals (and every persisted value) stay
    // stable. `expired` is a terminal state distinct from an operator cancel
    // (end-of-stream expiry, venue TIF expiry). `unknown` is the state of an
    // order id the ledger has never seen.
    expired,
    unknown
};

// Open == still able to consume capacity or produce a fill. Terminal states
// never count as open orders and never carry pending exposure.
[[nodiscard]] constexpr bool order_status_is_open(order_status status) noexcept
{
    return status == order_status::pending
        || status == order_status::open
        || status == order_status::partially_filled;
}

[[nodiscard]] constexpr bool order_status_is_terminal(order_status status) noexcept
{
    return status == order_status::filled
        || status == order_status::cancelled
        || status == order_status::rejected
        || status == order_status::expired;
}

[[nodiscard]] inline const char* to_string(order_status status) noexcept
{
    switch (status)
    {
    case order_status::pending:          return "pending";
    case order_status::open:             return "open";
    case order_status::partially_filled: return "partial";
    case order_status::filled:           return "filled";
    case order_status::cancelled:        return "cancelled";
    case order_status::rejected:         return "rejected";
    case order_status::expired:          return "expired";
    case order_status::unknown:          return "unknown";
    }
    return "unknown";
}

// One authoritative record per engine order id. Deliberately POD-ish and
// symbol-interned (uint16 id, not std::string) so the long-lived ledger map
// stays small and no hot-path transition allocates.
struct order_ledger_entry
{
    std::uint64_t order_id = 0;
    std::uint16_t symbol_id = SymbolTable::kInvalidId;
    order_side    side = order_side::buy;
    order_type    type = order_type::market;
    order_status  status = order_status::unknown;

    double original_qty = 0.0;
    double filled_qty = 0.0;
    double limit_price = 0.0;

    std::chrono::system_clock::time_point created_ts{};
    std::chrono::system_clock::time_point updated_ts{};

    // Diagnostic correlation only.  Economic idempotency is based on the
    // monotone cumulative cursor, not on an evicting ID cache.
    std::uint64_t last_fill_id = 0;

    // Monotone engine-thread mutation generation.  Read-only admission
    // results bind to this value so an old validation token can never be
    // committed after an intervening lifecycle/amend/fill mutation.
    std::uint64_t revision = 0;

    // Invariant: 0 <= filled_qty <= original_qty, so this never goes negative.
    [[nodiscard]] double remaining_qty() const noexcept
    {
        const double remaining = original_qty - filled_qty;
        return remaining > 0.0 ? remaining : 0.0;
    }

    [[nodiscard]] bool is_open() const noexcept
    {
        return order_status_is_open(status);
    }
};

// Per-symbol aggregate of everything still able to fill. Maintained
// incrementally on every lifecycle transition, so a pre-trade query is O(1)
// and allocation-free.
struct symbol_open_exposure
{
    double open_buy_qty = 0.0;    // Σ remaining_qty over open BUY orders
    double open_sell_qty = 0.0;   // Σ remaining_qty over open SELL orders
    std::size_t open_order_count = 0;
};

enum class lifecycle_apply_code : std::uint8_t
{
    applied,
    duplicate,
    stale,
    unknown_order,
    invalid_timestamp,
    invalid_transition,
};

struct lifecycle_apply_result
{
    lifecycle_apply_code code = lifecycle_apply_code::invalid_transition;
    std::uint64_t order_id = 0;
    std::uint64_t expected_revision = 0;
    order_status before = order_status::unknown;
    order_status after = order_status::unknown;
    std::chrono::system_clock::time_point timestamp{};

    [[nodiscard]] bool applied() const noexcept
    {
        return code == lifecycle_apply_code::applied;
    }

    [[nodiscard]] bool benign_noop() const noexcept
    {
        return code == lifecycle_apply_code::duplicate
            || code == lifecycle_apply_code::stale;
    }
};

// Result of the read-only economic-fill admission pass.  Replays/stale
// deliveries are successful no-ops; every rejected code is a reconciliation
// fault and must leave the ledger and all downstream economic state untouched.
enum class fill_apply_code : std::uint8_t
{
    applied,
    duplicate,
    stale,
    unknown_order,
    invalid_identity,
    invalid_value,
    inconsistent_quantity,
    disallowed_state,
    native_identity_conflict,
    native_identity_capacity_exhausted
};

[[nodiscard]] constexpr const char* to_string(fill_apply_code code) noexcept
{
    switch (code)
    {
    case fill_apply_code::applied:               return "applied";
    case fill_apply_code::duplicate:             return "duplicate";
    case fill_apply_code::stale:                 return "stale";
    case fill_apply_code::unknown_order:         return "unknown_order";
    case fill_apply_code::invalid_identity:      return "invalid_identity";
    case fill_apply_code::invalid_value:         return "invalid_value";
    case fill_apply_code::inconsistent_quantity: return "inconsistent_quantity";
    case fill_apply_code::disallowed_state:      return "disallowed_state";
    case fill_apply_code::native_identity_conflict:
        return "native_identity_conflict";
    case fill_apply_code::native_identity_capacity_exhausted:
        return "native_identity_capacity_exhausted";
    }
    return "invalid_value";
}

// Fixed-size economic identity carried by fill-admission tokens and by the
// non-evicting native-execution registry. Local fill_id is deliberately not
// included: it identifies a transport delivery, while venue_execution_id
// identifies the economic execution across reconnects and event-log replay.
struct fill_execution_fingerprint
{
    std::uint64_t order_id = 0;
    std::uint64_t local_economic_fill_id = 0;
    std::uint16_t symbol_id = SymbolTable::kInvalidId;
    order_side side = order_side::buy;
    fill_source source = fill_source::unknown;
    fill_cumulative_source cumulative_source =
        fill_cumulative_source::absent;
    double quantity = 0.0;
    double price = 0.0;
    double commission = 0.0;
    double remaining = 0.0;
    double cumulative = 0.0;
    std::int64_t timestamp_ticks = 0;
    std::array<char, 96> venue_execution_id{};
    std::uint8_t venue_execution_id_size = 0;
    std::array<char, 24> commission_currency{};
    std::uint8_t commission_currency_size = 0;

    [[nodiscard]] bool operator==(
        const fill_execution_fingerprint&) const noexcept = default;
};

struct fill_apply_result
{
    fill_apply_code code = fill_apply_code::invalid_value;
    std::uint64_t order_id = 0;
    std::uint64_t expected_revision = 0;
    order_status before = order_status::unknown;
    order_status after = order_status::unknown;
    double cumulative_qty = 0.0;
    double economic_quantity = 0.0;
    fill_execution_fingerprint fingerprint{};
    bool require_exchange_identity = false;
    bool require_fill_identity = false;

    [[nodiscard]] bool applied() const noexcept
    {
        return code == fill_apply_code::applied;
    }

    [[nodiscard]] bool idempotent_noop() const noexcept
    {
        return code == fill_apply_code::duplicate
            || code == fill_apply_code::stale;
    }

    [[nodiscard]] bool rejected() const noexcept
    {
        return !applied() && !idempotent_noop();
    }
};

enum class amend_apply_code : std::uint8_t
{
    applied,
    unknown_order,
    invalid_identity,
    invalid_value,
    disallowed_state,
    unsupported_order_type,
    quantity_not_above_filled,
    stale_validation
};

[[nodiscard]] constexpr const char* to_string(amend_apply_code code) noexcept
{
    switch (code)
    {
    case amend_apply_code::applied:                   return "applied";
    case amend_apply_code::unknown_order:             return "unknown_order";
    case amend_apply_code::invalid_identity:          return "invalid_identity";
    case amend_apply_code::invalid_value:             return "invalid_value";
    case amend_apply_code::disallowed_state:          return "disallowed_state";
    case amend_apply_code::unsupported_order_type:    return "unsupported_order_type";
    case amend_apply_code::quantity_not_above_filled: return "quantity_not_above_filled";
    case amend_apply_code::stale_validation:          return "stale_validation";
    }
    return "invalid_value";
}

// Read-only amend admission token.  The public quantity contract is a new
// TOTAL order quantity; remaining quantity is derived from the authoritative
// cumulative fill cursor.  Binding the token to the ledger revision prevents
// a preflight made before a crossing fill/lifecycle transition from being
// committed afterward.
struct amend_apply_result
{
    amend_apply_code code = amend_apply_code::invalid_value;
    std::uint64_t order_id = 0;
    std::uint64_t expected_revision = 0;
    order_status status = order_status::unknown;
    double old_price = 0.0;
    double old_total_qty = 0.0;
    double filled_qty = 0.0;
    double new_price = 0.0;
    double new_total_qty = 0.0;

    [[nodiscard]] bool applied() const noexcept
    {
        return code == amend_apply_code::applied;
    }

    [[nodiscard]] double new_remaining_qty() const noexcept
    {
        return new_total_qty - filled_qty;
    }
};

class OrderTracker
{
public:
    static constexpr double qty_epsilon = 1e-12;
    static constexpr std::size_t default_native_execution_capacity = 16384;

    explicit OrderTracker(
        std::size_t native_execution_capacity =
            default_native_execution_capacity,
        double quantity_quantum = 0.0)
        : native_execution_capacity_(native_execution_capacity)
        , quantity_quantum_(quantity_quantum)
    {
        if (!std::isfinite(quantity_quantum) || quantity_quantum < 0.0)
            throw std::invalid_argument("quantity quantum must be finite and nonnegative");
        if (native_execution_capacity
            > (std::numeric_limits<std::size_t>::max() / 2))
            throw std::invalid_argument(
                "native execution capacity is too large");
        const auto requested_slots = std::max<std::size_t>(
            2, native_execution_capacity * 2);
        native_executions_.resize(std::bit_ceil(requested_slots));
    }

    // ---- authoritative mutation path -------------------------------------

    // Record/refresh an order's identity and terms. Never changes status on
    // its own: route() reserves the lifecycle slot with set_status() after
    // its own capacity check, and re-registration (stop conversion, pending
    // release, instrument-spec rounding) must not resurrect a terminal order
    // or drop already-filled quantity.
    bool register_order(const order_event& order)
    {
        const auto order_id = order.get_order_id();
        const double qty = order.get_quantity();
        const double price = order.get_price();
        if (order_id == 0 || order.get_symbol().empty()
            || !std::isfinite(qty) || !(qty > 0.0)
            || !std::isfinite(price))
            return false;

        auto existing = orders_.find(order_id);
        if (existing != orders_.end())
        {
            const auto& current = existing->second;
            const bool same_terms =
                current.type == order.get_order_type()
                && current.original_qty == qty
                && current.limit_price == price;
            const bool allowed_staged_conversion =
                current.status == order_status::pending
                && current.filled_qty == 0.0
                && current.original_qty == qty
                && ((current.type == order_type::stop
                     && order.get_order_type() == order_type::market)
                    || (current.type == order_type::stop_limit
                        && order.get_order_type() == order_type::limit));
            if (order_status_is_terminal(current.status)
                || (current.symbol_id != SymbolTable::kInvalidId
                 && symbol_of(current) != order.get_symbol())
                || current.side != order.get_side()
                || qty + qty_epsilon < current.filled_qty
                || (!same_terms && !allowed_staged_conversion))
                return false;
        }

        const auto symbol_id = intern(order.get_symbol());
        if (symbol_id == SymbolTable::kInvalidId)
            return false;

        auto& entry = orders_[order_id];
        const auto before = snapshot_contribution(entry);

        entry.order_id = order_id;
        entry.symbol_id = symbol_id;
        entry.side = order.get_side();
        entry.type = order.get_order_type();
        entry.limit_price = price;
        if (entry.created_ts.time_since_epoch().count() == 0)
            entry.created_ts = order.get_timestamp();
        entry.updated_ts = order.get_timestamp();

        entry.original_qty = qty;
        ++entry.revision;

        commit_contribution(entry, before);
        return true;
    }

    void set_status(std::uint64_t order_id, order_status status)
    {
        auto found = orders_.find(order_id);
        if (found == orders_.end())
            return;
        auto& entry = found->second;
        const auto before = snapshot_contribution(entry);
        entry.order_id = order_id;

        // Lifecycle reports can cross on the wire.  A delayed ACK/cancel
        // must never resurrect or overwrite an already-terminal economic
        // state, and a delayed ACK after a partial fill must not erase the
        // partial state.  Rich venue-transition validation is performed by
        // the ingress seam; these guards keep the authoritative ledger safe
        // even for replay and legacy callers.
        if (order_status_is_terminal(entry.status))
        {
            commit_contribution(entry, before);
            return;
        }
        if (entry.status == order_status::partially_filled
            && (status == order_status::pending || status == order_status::open))
        {
            commit_contribution(entry, before);
            return;
        }
        if (entry.status != status)
        {
            entry.status = status;
            ++entry.revision;
        }
        commit_contribution(entry, before);
    }

    [[nodiscard]] lifecycle_apply_result validate_lifecycle(
        std::uint64_t order_id,
        order_status target,
        std::chrono::system_clock::time_point timestamp) const noexcept
    {
        lifecycle_apply_result result;
        result.order_id = order_id;
        result.after = target;
        result.timestamp = timestamp;

        const auto found = orders_.find(order_id);
        if (found == orders_.end())
        {
            result.code = lifecycle_apply_code::unknown_order;
            return result;
        }
        const auto& entry = found->second;
        result.expected_revision = entry.revision;
        result.before = entry.status;

        if (timestamp.time_since_epoch().count() <= 0)
        {
            result.code = lifecycle_apply_code::invalid_timestamp;
            return result;
        }
        if (entry.status == target)
        {
            result.code = lifecycle_apply_code::duplicate;
            return result;
        }
        if (entry.status == order_status::filled)
        {
            result.code = lifecycle_apply_code::stale;
            return result;
        }
        // A fill may be delivered before an older venue ACK.  Once economic
        // quantity has advanced, reopening the order adds no information and
        // must not turn a valid reconnect replay into a terminal timestamp
        // error.
        if (entry.status == order_status::partially_filled
            && target == order_status::open)
        {
            result.code = lifecycle_apply_code::stale;
            return result;
        }
        if (order_status_is_terminal(entry.status))
        {
            result.code = (target == order_status::open)
                ? lifecycle_apply_code::stale
                : lifecycle_apply_code::invalid_transition;
            return result;
        }
        if ((entry.created_ts.time_since_epoch().count() > 0
             && timestamp < entry.created_ts)
            || (entry.updated_ts.time_since_epoch().count() > 0
                && timestamp < entry.updated_ts))
        {
            result.code = lifecycle_apply_code::invalid_timestamp;
            return result;
        }
        if (target == order_status::open)
        {
            result.code = entry.status == order_status::pending
                ? lifecycle_apply_code::applied
                : lifecycle_apply_code::stale;
            return result;
        }
        if (target == order_status::rejected)
        {
            result.code = entry.status == order_status::pending
                ? lifecycle_apply_code::applied
                : lifecycle_apply_code::invalid_transition;
            return result;
        }
        if (target == order_status::cancelled
            || target == order_status::expired)
        {
            result.code = order_status_is_open(entry.status)
                ? lifecycle_apply_code::applied
                : lifecycle_apply_code::invalid_transition;
            return result;
        }
        result.code = lifecycle_apply_code::invalid_transition;
        return result;
    }

    bool commit_lifecycle(const lifecycle_apply_result& validated) noexcept
    {
        if (!validated.applied()) return false;
        const auto found = orders_.find(validated.order_id);
        if (found == orders_.end()) return false;
        auto& entry = found->second;
        if (entry.revision != validated.expected_revision)
            return false;
        const auto current = validate_lifecycle(
            validated.order_id, validated.after, validated.timestamp);
        if (!current.applied()
            || current.expected_revision != validated.expected_revision
            || current.before != validated.before
            || current.after != validated.after)
            return false;

        const auto before = snapshot_contribution(entry);
        entry.status = validated.after;
        entry.updated_ts = validated.timestamp;
        ++entry.revision;
        commit_contribution(entry, before);
        return true;
    }

    [[nodiscard]] amend_apply_result validate_amend(
        std::uint64_t order_id,
        const std::string& symbol,
        double new_price,
        double new_total_qty) const noexcept
    {
        amend_apply_result result;
        auto it = orders_.find(order_id);
        if (it == orders_.end())
        {
            result.code = amend_apply_code::unknown_order;
            return result;
        }
        const auto& entry = it->second;
        result.order_id = entry.order_id;
        result.expected_revision = entry.revision;
        result.status = entry.status;
        result.old_price = entry.limit_price;
        result.old_total_qty = entry.original_qty;
        result.filled_qty = entry.filled_qty;
        result.new_price = new_price;
        result.new_total_qty = new_total_qty;

        if (symbol.empty() || symbol_of(entry) != symbol)
        {
            result.code = amend_apply_code::invalid_identity;
            return result;
        }
        if (!std::isfinite(new_price) || !(new_price > 0.0)
            || !std::isfinite(new_total_qty) || !(new_total_qty > 0.0))
        {
            result.code = amend_apply_code::invalid_value;
            return result;
        }
        if (entry.status != order_status::open
            && entry.status != order_status::partially_filled)
        {
            result.code = amend_apply_code::disallowed_state;
            return result;
        }
        if (entry.type != order_type::limit)
        {
            result.code = amend_apply_code::unsupported_order_type;
            return result;
        }
        if (new_total_qty <= entry.filled_qty + qty_epsilon)
        {
            result.code = amend_apply_code::quantity_not_above_filled;
            return result;
        }

        result.code = amend_apply_code::applied;
        return result;
    }

    bool commit_amend(const amend_apply_result& validated) noexcept
    {
        if (!validated.applied())
            return false;
        auto it = orders_.find(validated.order_id);
        if (it == orders_.end())
            return false;
        auto& entry = it->second;
        if (entry.revision != validated.expected_revision)
            return false;

        const auto current = validate_amend(
            validated.order_id, symbol_of(entry), validated.new_price,
            validated.new_total_qty);
        if (!current.applied()
            || current.expected_revision != validated.expected_revision
            || current.status != validated.status
            || current.old_price != validated.old_price
            || current.old_total_qty != validated.old_total_qty
            || current.filled_qty != validated.filled_qty
            || current.new_price != validated.new_price
            || current.new_total_qty != validated.new_total_qty)
            return false;

        const auto before = snapshot_contribution(entry);
        entry.limit_price = validated.new_price;
        entry.original_qty = validated.new_total_qty;
        ++entry.revision;
        commit_contribution(entry, before);
        return true;
    }

    // Compatibility entry point for replay/tests. Production uses the split
    // validate/commit form so all instrument/risk checks and fallible resource
    // reservations happen before any adapter mutation.
    bool amend(std::uint64_t order_id, double new_price, double new_total_qty)
    {
        const auto it = orders_.find(order_id);
        if (it == orders_.end())
            return false;
        return commit_amend(validate_amend(
            order_id, symbol_of(it->second), new_price, new_total_qty));
    }

    // Canonical fill application: accumulates filled quantity under the
    // ledger invariants, drops duplicate fill events, and derives the
    // resulting lifecycle state. Returns false when the fill was ignored as
    // a duplicate.
    [[nodiscard]] fill_apply_result validate_fill(
        const fill_event& fill,
        bool require_exchange_identity = false,
        bool require_fill_identity = false) const noexcept
    {
        fill_apply_result result;
        result.require_exchange_identity = require_exchange_identity;
        result.require_fill_identity = require_fill_identity;
        const auto it = orders_.find(fill.get_order_id());
        if (it == orders_.end())
        {
            result.code = fill_apply_code::unknown_order;
            return result;
        }
        const auto& entry = it->second;
        result.order_id = entry.order_id;
        result.expected_revision = entry.revision;
        result.before = entry.status;
        result.after = entry.status;

        const auto& expected_symbol = symbol_of(entry);
        if (fill.get_symbol().empty() || expected_symbol.empty()
            || fill.get_symbol() != expected_symbol
            || fill.get_side() != entry.side)
        {
            result.code = fill_apply_code::invalid_identity;
            return result;
        }

        if ((require_fill_identity && fill.get_fill_id() == 0)
            || (require_exchange_identity
            && fill.get_source() != fill_source::exchange)
            )
        {
            result.code = fill_apply_code::invalid_identity;
            return result;
        }

        const bool native_execution = require_exchange_identity
            || fill.get_source() == fill_source::exchange;
        if (native_execution
            && (fill.get_fill_id() == 0
                || fill.get_venue_execution_id().empty()
                || fill.get_commission_currency().empty()
                || !fill.has_cumulative_filled_qty()))
        {
            result.code = fill_apply_code::invalid_identity;
            return result;
        }
        if (native_execution
            && (fill.get_timestamp().time_since_epoch().count() <= 0
                || (entry.created_ts.time_since_epoch().count() > 0
                    && fill.get_timestamp() < entry.created_ts)))
        {
            result.code = fill_apply_code::invalid_value;
            return result;
        }

        const double qty = fill.get_reported_filled_quantity();
        const double price = fill.get_fill_price();
        const double commission = fill.get_commission();
        const double reported_remaining = fill.get_remaining_qty();
        if (!std::isfinite(qty) || !(qty > 0.0)
            || !std::isfinite(price) || !(price > 0.0)
            || !std::isfinite(commission)
            || (commission != 0.0
                && fill.get_commission_currency().empty())
            || !std::isfinite(reported_remaining)
            || reported_remaining < 0.0
            || !std::isfinite(entry.original_qty)
            || !(entry.original_qty > 0.0))
        {
            result.code = fill_apply_code::invalid_value;
            return result;
        }

        const double notional = qty * price;
        if (!std::isfinite(notional)
            || !std::isfinite(notional + commission)
            || !std::isfinite(notional - commission))
        {
            result.code = fill_apply_code::invalid_value;
            return result;
        }

        const double cumulative = fill.has_cumulative_filled_qty()
            ? fill.get_cumulative_filled_qty()
            : entry.original_qty - reported_remaining;
        if (!std::isfinite(cumulative))
        {
            result.code = fill_apply_code::invalid_value;
            return result;
        }

        result.fingerprint = make_fingerprint(
            fill, entry.symbol_id, cumulative);
        if (native_execution)
        {
            const auto native_probe = probe_native(result.fingerprint);
            if (native_probe.exact_match)
            {
                result.code = fill_apply_code::duplicate;
                return result;
            }
            if (native_probe.key_conflict)
            {
                result.code = fill_apply_code::native_identity_conflict;
                return result;
            }
            if (native_execution_count_ >= native_execution_capacity_
                || native_probe.index == native_npos)
            {
                result.code =
                    fill_apply_code::native_identity_capacity_exhausted;
                return result;
            }
        }

        if (entry.status == order_status::unknown
            || entry.status == order_status::rejected)
        {
            result.code = fill_apply_code::disallowed_state;
            return result;
        }

        // Current adapters report the remainder *after* this slice.  That
        // makes original-remaining the cumulative economic cursor.  It is
        // monotone, survives arbitrarily old reconnect replays, and cannot
        // silently synthesize a missing slice: every forward step must equal
        // exactly this fill's delta.  Venue ingress is being extended with an
        // explicit native cumulative field; until then this invariant is the
        // authoritative representation carried by fill_event.
        const double expected = entry.filled_qty + qty;
        const double cursor_tolerance = bounded_quantity_tolerance(
            quantity_tolerance(
                cumulative, expected, entry.filled_qty, qty));
        const double remaining_tolerance = bounded_quantity_tolerance(
            quantity_tolerance(
                entry.original_qty, entry.original_qty - cumulative,
                reported_remaining, qty));
        if (!std::isfinite(cumulative)
            || cumulative < 0.0
            // The venue/simulator cumulative cursor is authoritative and may
            // never cross the order total by even one representable value.
            || cumulative > entry.original_qty
            // The slice sum itself may differ from the authoritative cursor
            // by the rounding error of one binary addition (e.g. 0.1+0.2).
            // It is normalized to the exact cursor delta before downstream
            // accounting; anything outside that bound remains an overfill.
            || expected - entry.original_qty > cursor_tolerance)
        {
            result.code = fill_apply_code::inconsistent_quantity;
            return result;
        }

        if (fill.has_cumulative_filled_qty()
            && std::abs((entry.original_qty - cumulative)
                        - reported_remaining) > remaining_tolerance)
        {
            result.code = fill_apply_code::inconsistent_quantity;
            return result;
        }

        if (!native_execution && fill.get_fill_id() != 0
            && entry.last_fill_id != 0
            && fill.get_fill_id() <= entry.last_fill_id
            && cumulative > entry.filled_qty + cursor_tolerance)
        {
            result.code = fill_apply_code::native_identity_conflict;
            return result;
        }

        result.cumulative_qty = cumulative;
        if (result.cumulative_qty < entry.filled_qty - cursor_tolerance)
        {
            result.code = native_execution
                ? fill_apply_code::inconsistent_quantity
                : fill_apply_code::stale;
            return result;
        }
        if (std::abs(result.cumulative_qty - entry.filled_qty)
            <= cursor_tolerance)
        {
            result.code = native_execution
                ? fill_apply_code::inconsistent_quantity
                : fill_apply_code::duplicate;
            return result;
        }
        if (std::abs(result.cumulative_qty - expected) > cursor_tolerance)
        {
            result.code = fill_apply_code::inconsistent_quantity;
            return result;
        }

        result.economic_quantity =
            result.cumulative_qty - entry.filled_qty;
        if (!std::isfinite(result.economic_quantity)
            || !(result.economic_quantity > 0.0))
        {
            result.code = fill_apply_code::inconsistent_quantity;
            return result;
        }

        result.code = fill_apply_code::applied;
        if (!order_status_is_terminal(entry.status))
        {
            result.after = result.cumulative_qty < entry.original_qty
                ? order_status::partially_filled
                : order_status::filled;
        }
        return result;
    }

    bool commit_fill(const fill_event& fill,
                     const fill_apply_result& validated) noexcept
    {
        if (!validated.applied())
            return false;
        auto it = orders_.find(fill.get_order_id());
        if (it == orders_.end())
            return false;
        auto& entry = it->second;
        if (validated.order_id != fill.get_order_id()
            || validated.expected_revision != entry.revision)
            return false;

        // Re-run the pure admission calculation at the commit boundary and
        // require the caller's token to describe exactly this ledger state.
        // This prevents a token validated for one order/cursor from rolling
        // another order—or a later revision of the same order—backward.
        const auto current = validate_fill(
            fill, validated.require_exchange_identity,
            validated.require_fill_identity);
        if (!current.applied()
            || current.order_id != validated.order_id
            || current.expected_revision != validated.expected_revision
            || current.before != validated.before
            || current.after != validated.after
            || current.cumulative_qty != validated.cumulative_qty
            || current.economic_quantity != validated.economic_quantity
            || !(current.fingerprint == validated.fingerprint))
            return false;

        std::size_t native_insert_index = native_npos;
        if (validated.require_exchange_identity
            || fill.get_source() == fill_source::exchange)
        {
            const auto native_probe = probe_native(validated.fingerprint);
            if (native_probe.exact_match || native_probe.key_conflict
                || native_probe.index == native_npos
                || native_execution_count_ >= native_execution_capacity_)
                return false;
            native_insert_index = native_probe.index;
        }

        const auto before = snapshot_contribution(entry);
        entry.filled_qty = validated.cumulative_qty;
        entry.updated_ts = fill.get_timestamp();
        entry.last_fill_id = fill.get_fill_id();
        entry.status = validated.after;
        ++entry.revision;
        commit_contribution(entry, before);
        if (native_insert_index != native_npos)
        {
            auto& slot = native_executions_[native_insert_index];
            slot.occupied = true;
            slot.hash = native_key_hash(validated.fingerprint);
            slot.fingerprint = validated.fingerprint;
            ++native_execution_count_;
        }
        return true;
    }

    // Compatibility entry point for tests and leaf accounting helpers.  The
    // engine uses validate_fill()+commit_fill() so all non-ledger prechecks
    // happen before the single economic commit point.
    bool on_fill(const fill_event& fill)
    {
        const auto validation = validate_fill(fill);
        return commit_fill(fill, validation);
    }

    // Read-only duplicate probe for callers that must validate a fill before
    // they can safely mutate another ledger (portfolio/bracket state). It
    // deliberately mirrors on_fill's bounded per-order fill-id window but
    // never creates an entry or advances the ring slot.
    bool has_seen_fill(std::uint64_t order_id, std::uint64_t fill_id) const
    {
        if (fill_id == 0)
            return false;
        const auto it = orders_.find(order_id);
        if (it == orders_.end())
            return false;
        return it->second.last_fill_id == fill_id;
    }

    // ---- queries ---------------------------------------------------------

    order_status get_order_status(std::uint64_t order_id) const
    {
        auto it = orders_.find(order_id);
        return (it != orders_.end()) ? it->second.status : order_status::unknown;
    }

    std::vector<std::uint64_t> get_open_orders() const
    {
        std::vector<std::uint64_t> result;
        result.reserve(active_count());
        for (const auto& [id, entry] : orders_)
            if (entry.is_open())
                result.push_back(id);
        return result;
    }

    bool is_active(std::uint64_t order_id) const
    {
        const auto it = orders_.find(order_id);
        return it != orders_.end() && it->second.is_open();
    }

    std::size_t active_count() const
    {
        return active_count_.load(std::memory_order_acquire);
    }

    // This is the only OrderTracker state workers may read. The ledger
    // remains engine-thread-owned; exposing the atomic avoids concurrent map
    // access.
    const std::atomic<std::size_t>& active_count_atomic() const
    {
        return active_count_;
    }

    const order_ledger_entry* find(std::uint64_t order_id) const
    {
        auto it = orders_.find(order_id);
        return (it != orders_.end()) ? &it->second : nullptr;
    }

    // Quantity that can still fill: the entry's arithmetic remainder while
    // the order is open, and exactly 0 once it is terminal. This is the
    // number risk means by "pending" — order_ledger_entry::remaining_qty()
    // is the raw original-minus-filled invariant and keeps its value on a
    // cancelled or expired order.
    double pending_qty(std::uint64_t order_id) const
    {
        const auto* entry = find(order_id);
        return (entry && entry->is_open()) ? entry->remaining_qty() : 0.0;
    }

    double filled_qty(std::uint64_t order_id) const
    {
        const auto* entry = find(order_id);
        return entry ? entry->filled_qty : 0.0;
    }

    // O(1). Returns a zeroed aggregate for a symbol the ledger has never
    // seen; use tracks_symbol() to distinguish "flat" from "not tracked".
    symbol_open_exposure open_exposure(const std::string& symbol) const
    {
        const auto id = symbols_.id_of(symbol);
        if (id == SymbolTable::kInvalidId || id >= exposure_.size())
            return {};
        return exposure_[id];
    }

    bool tracks_symbol(const std::string& symbol) const
    {
        return symbols_.id_of(symbol) != SymbolTable::kInvalidId;
    }

    // True once the interning table is full: further symbols get no per-symbol
    // aggregate, so a per-symbol limit must fail closed rather than read zero.
    bool symbol_capacity_exhausted() const
    {
        return symbols_.size() >= SymbolTable::kMaxSymbols;
    }

    // Lifetime count of distinct order ids the ledger has recorded. Replaces
    // "open orders + fills" style reporting derivations.
    std::size_t orders_seen() const { return orders_.size(); }

    std::size_t native_execution_count() const noexcept
    {
        return native_execution_count_;
    }

    template <typename Fn>
    void for_each_open(Fn&& fn) const
    {
        for (const auto& [id, entry] : orders_)
            if (entry.is_open())
                fn(entry);
    }

    // Cold path only: O(every order the ledger has ever seen). Used by
    // end-of-run reporting and tests; never call this per event.
    template <typename Fn>
    void for_each_order(Fn&& fn) const
    {
        for (const auto& [id, entry] : orders_)
            fn(entry);
    }

    // O(#distinct symbols) — bounded by SymbolTable::kMaxSymbols and in
    // practice 1-5. This is the iteration a per-event risk snapshot uses; it
    // must never degrade to "every order ever seen".
    template <typename Fn>
    void for_each_symbol_exposure(Fn&& fn) const
    {
        for (std::uint16_t id = 0; id < exposure_.size(); ++id)
            fn(symbols_.resolve(id), exposure_[id]);
    }

    const std::string& symbol_of(const order_ledger_entry& entry) const
    {
        static const std::string empty;
        return (entry.symbol_id != SymbolTable::kInvalidId)
            ? symbols_.resolve(entry.symbol_id) : empty;
    }

    // Cold path: sized once at run start so no hot-path transition rehashes.
    void reserve(std::size_t orders)
    {
        if (orders > 0)
            orders_.reserve(orders);
        exposure_.reserve(SymbolTable::kMaxSymbols);
    }

    // Phase A (MC object reuse)
    void reset()
    {
        orders_.clear();
        exposure_.clear();
        symbols_.clear();
        for (auto& slot : native_executions_)
            slot = {};
        native_execution_count_ = 0;
        active_count_.store(0, std::memory_order_release);
    }

private:
    static constexpr std::size_t native_npos =
        std::numeric_limits<std::size_t>::max();

    static double quantity_tolerance(double original,
                                     double cumulative,
                                     double expected,
                                     double remaining) noexcept
    {
        const double scale = std::max(
            {1.0, std::abs(original), std::abs(cumulative),
             std::abs(expected), std::abs(remaining)});
        const double next = std::nextafter(
            scale, std::numeric_limits<double>::infinity());
        const double ulp = next - scale;
        // Two representable steps cover one rounded addition/subtraction at
        // this magnitude. Unlike a relative 1e-9 epsilon, this can never turn
        // whole tradable units into duplicates or admit a material overfill.
        return std::isfinite(ulp) ? 2.0 * ulp : 0.0;
    }

    double bounded_quantity_tolerance(double ulp_tolerance) const noexcept
    {
        if (!(quantity_quantum_ > 0.0))
            return 0.0;
        // A correction can only explain binary representation of a value on
        // the configured quantity grid. Half a tradable quantum is the hard
        // domain boundary; this prevents large-double ULPs from normalizing
        // hundreds of real units into an order total.
        return std::min(ulp_tolerance, quantity_quantum_ * 0.5);
    }

    struct native_execution_slot
    {
        bool occupied = false;
        std::uint64_t hash = 0;
        fill_execution_fingerprint fingerprint{};
    };

    struct native_probe_result
    {
        std::size_t index = native_npos;
        bool exact_match = false;
        bool key_conflict = false;
    };

    static fill_execution_fingerprint make_fingerprint(
        const fill_event& fill,
        std::uint16_t symbol_id,
        double cumulative) noexcept
    {
        fill_execution_fingerprint fingerprint;
        fingerprint.order_id = fill.get_order_id();
        fingerprint.local_economic_fill_id =
            fill.get_source() == fill_source::exchange
                ? 0
                : fill.get_fill_id();
        fingerprint.symbol_id = symbol_id;
        fingerprint.side = fill.get_side();
        fingerprint.source = fill.get_source();
        fingerprint.cumulative_source = fill.get_cumulative_source();
        fingerprint.quantity = fill.get_reported_filled_quantity();
        fingerprint.price = fill.get_fill_price();
        fingerprint.commission = fill.get_commission();
        fingerprint.remaining = fill.get_remaining_qty();
        fingerprint.cumulative = cumulative;
        fingerprint.timestamp_ticks = static_cast<std::int64_t>(
            fill.get_timestamp().time_since_epoch().count());

        const auto native_id = fill.get_venue_execution_id();
        fingerprint.venue_execution_id_size =
            static_cast<std::uint8_t>(native_id.size());
        std::copy(native_id.begin(), native_id.end(),
                  fingerprint.venue_execution_id.begin());

        const auto currency = fill.get_commission_currency();
        fingerprint.commission_currency_size =
            static_cast<std::uint8_t>(currency.size());
        std::copy(currency.begin(), currency.end(),
                  fingerprint.commission_currency.begin());
        return fingerprint;
    }

    static std::uint64_t native_key_hash(
        const fill_execution_fingerprint& fingerprint) noexcept
    {
        std::uint64_t hash = 1469598103934665603ULL;
        const auto mix = [&hash](std::uint8_t byte) noexcept
        {
            hash ^= byte;
            hash *= 1099511628211ULL;
        };
        if (fingerprint.venue_execution_id_size == 0)
        {
            for (unsigned shift = 0; shift < 64; shift += 8)
                mix(static_cast<std::uint8_t>(
                    fingerprint.order_id >> shift));
            for (unsigned shift = 0; shift < 64; shift += 8)
                mix(static_cast<std::uint8_t>(
                    fingerprint.local_economic_fill_id >> shift));
        }
        else
        {
            mix(static_cast<std::uint8_t>(fingerprint.symbol_id));
            mix(static_cast<std::uint8_t>(fingerprint.symbol_id >> 8));
        }
        for (std::size_t i = 0;
             i < fingerprint.venue_execution_id_size; ++i)
            mix(static_cast<std::uint8_t>(
                fingerprint.venue_execution_id[i]));
        return hash;
    }

    static bool same_native_key(
        const fill_execution_fingerprint& lhs,
        const fill_execution_fingerprint& rhs) noexcept
    {
        if (lhs.venue_execution_id_size != rhs.venue_execution_id_size)
            return false;
        if (lhs.venue_execution_id_size == 0)
            return lhs.order_id == rhs.order_id
                && lhs.local_economic_fill_id
                == rhs.local_economic_fill_id;
        return lhs.symbol_id == rhs.symbol_id
            && std::equal(
            lhs.venue_execution_id.begin(),
            lhs.venue_execution_id.begin()
                + lhs.venue_execution_id_size,
            rhs.venue_execution_id.begin());
    }

    native_probe_result probe_native(
        const fill_execution_fingerprint& fingerprint) const noexcept
    {
        if (native_executions_.empty())
            return {};
        const auto hash = native_key_hash(fingerprint);
        const auto mask = native_executions_.size() - 1;
        auto index = static_cast<std::size_t>(hash) & mask;
        for (std::size_t visited = 0;
             visited < native_executions_.size(); ++visited)
        {
            const auto& slot = native_executions_[index];
            if (!slot.occupied)
                return {index, false, false};
            if (slot.hash == hash
                && same_native_key(slot.fingerprint, fingerprint))
            {
                return {index, slot.fingerprint == fingerprint,
                        !(slot.fingerprint == fingerprint)};
            }
            index = (index + 1) & mask;
        }
        return {};
    }

    // What an entry currently contributes to the aggregates, captured before
    // a mutation so the commit is a pure delta and cannot drift.
    struct contribution
    {
        bool open = false;
        double remaining = 0.0;
        std::uint16_t symbol_id = SymbolTable::kInvalidId;
        order_side side = order_side::buy;
    };

    static contribution snapshot_contribution(const order_ledger_entry& entry)
    {
        contribution c;
        c.open = entry.is_open();
        c.remaining = c.open ? entry.remaining_qty() : 0.0;
        c.symbol_id = entry.symbol_id;
        c.side = entry.side;
        return c;
    }

    void commit_contribution(const order_ledger_entry& entry,
                             const contribution& before)
    {
        const contribution after = snapshot_contribution(entry);

        if (!before.open && after.open)
            active_count_.fetch_add(1, std::memory_order_release);
        else if (before.open && !after.open)
            active_count_.fetch_sub(1, std::memory_order_release);

        if (before.open && before.symbol_id != SymbolTable::kInvalidId)
            add_exposure(before.symbol_id, before.side, -before.remaining, -1);
        if (after.open && after.symbol_id != SymbolTable::kInvalidId)
            add_exposure(after.symbol_id, after.side, after.remaining, +1);
    }

    void add_exposure(std::uint16_t symbol_id, order_side side,
                      double qty_delta, int count_delta)
    {
        if (symbol_id >= exposure_.size())
            exposure_.resize(static_cast<std::size_t>(symbol_id) + 1);
        auto& agg = exposure_[symbol_id];
        double& bucket = (side == order_side::buy)
            ? agg.open_buy_qty : agg.open_sell_qty;
        bucket += qty_delta;
        if (bucket < qty_epsilon)
            bucket = 0.0;   // floating-point drift guard, never negative
        if (count_delta > 0)
            ++agg.open_order_count;
        else if (count_delta < 0 && agg.open_order_count > 0)
            --agg.open_order_count;
    }

    std::uint16_t intern(const std::string& symbol)
    {
        if (symbol.empty())
            return SymbolTable::kInvalidId;
        const auto existing = symbols_.id_of(symbol);
        if (existing != SymbolTable::kInvalidId)
            return existing;
        // Pre-check instead of catching: SymbolTable::intern_id throws when
        // full, and the ledger sits on the order hot path.
        if (symbols_.size() >= SymbolTable::kMaxSymbols)
            return SymbolTable::kInvalidId;
        return symbols_.intern_id(symbol);
    }

    std::unordered_map<std::uint64_t, order_ledger_entry> orders_;
    std::vector<symbol_open_exposure> exposure_;   // indexed by interned symbol id
    SymbolTable symbols_;
    std::vector<native_execution_slot> native_executions_;
    std::size_t native_execution_capacity_ = 0;
    std::size_t native_execution_count_ = 0;
    double quantity_quantum_ = 0.0;
    // Kept off the ledger's cache lines: written only by the engine and read
    // by workers as the authoritative pre-trade capacity snapshot.
    alignas(64) std::atomic<std::size_t> active_count_{0};
};
