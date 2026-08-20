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

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
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
    // Duplicate-fill guard. Re-delivery of the same venue fill (reconciler
    // replay, transport retry) arrives close together, so a small ring of the
    // most recent non-zero fill ids is enough and costs no allocation.
    static constexpr std::size_t fill_dedupe_slots = 4;

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

    std::array<std::uint64_t, fill_dedupe_slots> recent_fill_ids{};
    std::uint8_t fill_slot = 0;

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

class OrderTracker
{
public:
    static constexpr double qty_epsilon = 1e-12;

    // ---- authoritative mutation path -------------------------------------

    // Record/refresh an order's identity and terms. Never changes status on
    // its own: route() reserves the lifecycle slot with set_status() after
    // its own capacity check, and re-registration (stop conversion, pending
    // release, instrument-spec rounding) must not resurrect a terminal order
    // or drop already-filled quantity.
    void register_order(const order_event& order)
    {
        auto& entry = orders_[order.get_order_id()];
        const auto before = snapshot_contribution(entry);

        entry.order_id = order.get_order_id();
        entry.symbol_id = intern(order.get_symbol());
        entry.side = order.get_side();
        entry.type = order.get_order_type();
        entry.limit_price = order.get_price();
        if (entry.created_ts.time_since_epoch().count() == 0)
            entry.created_ts = order.get_timestamp();
        entry.updated_ts = order.get_timestamp();

        const double qty = order.get_quantity();
        entry.original_qty = (std::isfinite(qty) && qty > 0.0) ? qty : 0.0;
        clamp_fill(entry);

        commit_contribution(entry, before);
    }

    void set_status(std::uint64_t order_id, order_status status)
    {
        auto& entry = orders_[order_id];
        const auto before = snapshot_contribution(entry);
        entry.order_id = order_id;
        entry.status = status;
        commit_contribution(entry, before);
    }

    // Venue amendment: the resting order's terms changed, so the ledger's
    // notion of original quantity must follow. Without this a shrunk order
    // would never reach filled_qty == original_qty and would leak an open
    // slot forever.
    void amend(std::uint64_t order_id, double new_price, double new_qty)
    {
        auto it = orders_.find(order_id);
        if (it == orders_.end())
            return;
        auto& entry = it->second;
        const auto before = snapshot_contribution(entry);
        if (std::isfinite(new_price) && new_price > 0.0)
            entry.limit_price = new_price;
        if (std::isfinite(new_qty) && new_qty > 0.0)
            entry.original_qty = new_qty;
        clamp_fill(entry);
        commit_contribution(entry, before);
    }

    // Canonical fill application: accumulates filled quantity under the
    // ledger invariants, drops duplicate fill events, and derives the
    // resulting lifecycle state. Returns false when the fill was ignored as
    // a duplicate.
    bool on_fill(const fill_event& fill)
    {
        auto& entry = orders_[fill.get_order_id()];
        const auto before = snapshot_contribution(entry);

        if (entry.order_id == 0)
            entry.order_id = fill.get_order_id();
        if (entry.symbol_id == SymbolTable::kInvalidId)
            entry.symbol_id = intern(fill.get_symbol());
        if (entry.created_ts.time_since_epoch().count() == 0)
        {
            entry.created_ts = fill.get_timestamp();
            entry.side = fill.get_side();
        }

        const double qty = fill.get_filled_quantity();
        const double reported_remaining = fill.get_remaining_qty();

        if (!seen_fill(entry, fill.get_fill_id()))
        {
            // Nothing about this fill may be applied twice — not the
            // quantity and not the derived status.
            commit_contribution(entry, before);
            return false;
        }

        // An order the ledger never saw registered (legacy/test path, venue
        // fill for an unknown id) still gets a defensible original quantity
        // from the fill itself.
        if (entry.original_qty <= qty_epsilon)
        {
            const double implied = qty
                + (std::isfinite(reported_remaining) && reported_remaining > 0.0
                       ? reported_remaining : 0.0);
            entry.original_qty = implied > 0.0 ? implied : entry.original_qty;
        }

        if (std::isfinite(qty) && qty > 0.0)
            entry.filled_qty += qty;
        clamp_fill(entry);
        entry.updated_ts = fill.get_timestamp();

        // Partial if either the venue says so or the ledger still has
        // quantity outstanding — the fail-safe direction, because an order
        // wrongly marked filled would silently release its capacity slot and
        // its pending exposure while it can still trade.
        const bool venue_says_partial =
            std::isfinite(reported_remaining) && reported_remaining > qty_epsilon;
        entry.status = (venue_says_partial || entry.remaining_qty() > qty_epsilon)
            ? order_status::partially_filled
            : order_status::filled;

        commit_contribution(entry, before);
        return true;
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
        active_count_.store(0, std::memory_order_release);
    }

private:
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

    static void clamp_fill(order_ledger_entry& entry)
    {
        if (!(entry.filled_qty > 0.0))
            entry.filled_qty = 0.0;
        if (entry.filled_qty > entry.original_qty)
            entry.filled_qty = entry.original_qty;
    }

    // Returns true when the fill is new (and records it). fill_id == 0 means
    // the source does not mint fill ids (paper/simulated adapters), so there
    // is nothing to dedupe against and the fill is always applied.
    static bool seen_fill(order_ledger_entry& entry, std::uint64_t fill_id)
    {
        if (fill_id == 0)
            return true;
        for (auto id : entry.recent_fill_ids)
            if (id == fill_id)
                return false;
        entry.recent_fill_ids[entry.fill_slot] = fill_id;
        entry.fill_slot = static_cast<std::uint8_t>(
            (entry.fill_slot + 1) % order_ledger_entry::fill_dedupe_slots);
        return true;
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
    // Kept off the ledger's cache lines: written only by the engine and read
    // by workers as the authoritative pre-trade capacity snapshot.
    alignas(64) std::atomic<std::size_t> active_count_{0};
};
