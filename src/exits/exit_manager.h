#pragma once

#include "core/event.h"
#include "exits/exit_intent.h"
#include "exits/bracket_adapter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace truetest::exits {

// Layer-neutral representation of a native venue bracket-leg report.  The
// engine translates its fixed private-execution record into this type only
// after the ordinary execution bridge has proved that the report is not one
// of its own submitted orders.  Keeping this contract in exits (rather than
// including execution/private_execution_record.h) preserves the dependency
// graph: exits may depend only on core.
//
// `exchange_order_id` is the mandatory immutable lookup key.  A client id is
// supplementary provenance: adapters do not currently expose a canonical
// client-id handle for native legs, so it is fingerprinted for replay proof
// but never used as a fallback lookup.
enum class native_bracket_leg_role : std::uint8_t
{
    stop_loss,
    take_profit,
};

enum class native_bracket_lifecycle : std::uint8_t
{
    ack,
    partial_fill,
    full_fill,
    canceled,
    rejected,
    expired,
};

struct native_bracket_update
{
    native_bracket_lifecycle lifecycle = native_bracket_lifecycle::ack;
    std::string_view exchange_order_id;
    std::string_view client_order_id;
    std::string_view execution_id;
    std::string_view symbol;
    std::string_view group_id;
    std::string_view commission_asset;
    std::string_view error;
    order_side side = order_side::buy;

    std::int64_t event_time_ms = 0;
    // Copied from the provider-private FIFO. It must be non-zero and identify
    // this ingress record until engine-side accounting either commits or rolls
    // back the corresponding economic preflight reservation.
    std::uint64_t source_sequence = 0;
    double last_fill_qty = 0.0;
    double last_fill_price = 0.0;
    double cumulative_qty = 0.0;
    double commission = 0.0;
    bool cumulative_reported = false;
    bool lifecycle_only = false;

    [[nodiscard]] constexpr bool is_economic_fill() const noexcept
    {
        return (lifecycle == native_bracket_lifecycle::partial_fill
                || lifecycle == native_bracket_lifecycle::full_fill)
            && !lifecycle_only;
    }

    [[nodiscard]] constexpr bool is_terminal() const noexcept
    {
        return (lifecycle == native_bracket_lifecycle::full_fill
                && !lifecycle_only)
            || lifecycle == native_bracket_lifecycle::canceled
            || lifecycle == native_bracket_lifecycle::rejected
            || lifecycle == native_bracket_lifecycle::expired;
    }
};

enum class native_bracket_resolution_kind : std::uint8_t
{
    // No registered native leg has this exact exchange id.  This is not an
    // acceptance result; the caller must keep its generic unknown-order path
    // fail-closed.
    not_native,
    // Exact replay; no accounting or lifecycle work is required.
    duplicate,
    // A known non-economic lifecycle was accepted.
    lifecycle,
    // A known economic increment is attributable and must be accounted.
    economic_fill,
    // The increment is attributable and must be accounted, but it is an OCO
    // sibling fill after the opposite leg already won.  The caller must then
    // latch reconciliation rather than silently treating the second fill as
    // normal OCO behaviour.
    economic_fill_requires_reconciliation,
    // Identity, cumulative, replay, or protection proof failed.
    fatal,
};

// Opaque-in-practice, fixed-size proof that names one economic reservation.
// A provider FIFO sequence alone is not a safe commit key: an ExitManager may
// retain several native legs. `leg_token` is immutable for the retained leg,
// so commit/rollback must match both fields exactly.
struct native_bracket_economic_reservation
{
    std::uint64_t source_sequence = 0;
    std::uint64_t leg_token = 0;

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return source_sequence != 0 && leg_token != 0;
    }
};

struct native_bracket_resolution
{
    native_bracket_resolution_kind kind = native_bracket_resolution_kind::not_native;
    std::uint64_t opener_order_id = 0;
    native_bracket_leg_role role = native_bracket_leg_role::stop_loss;
    order_side close_side = order_side::sell;
    double remaining_qty = 0.0;
    bool terminal = false;
    // Valid only for an economic fill accepted by preflight. The engine must
    // commit this exact token after canonical accounting/audit, or roll it
    // back if accounting cannot complete.
    native_bracket_economic_reservation reservation;

    // References immutable, bounded ExitManager state.  Consume this before
    // the next native-bracket mutation; callers that need persistence copy it.
    std::string_view strategy_name;
};

enum class native_bracket_group_status : std::uint8_t
{
    active,
    completed,
};

struct native_bracket_group_update
{
    native_bracket_group_status status = native_bracket_group_status::active;
    std::string_view group_id;
    std::string_view symbol;
    std::int64_t event_time_ms = 0;
};

enum class native_bracket_group_resolution : std::uint8_t
{
    not_native,
    duplicate,
    lifecycle,
    fatal,
};

enum class native_bracket_sibling_cancel_result : std::uint8_t
{
    // No independent sibling exists, a prior cancel is already in flight, or
    // the venue owns atomic OCO cancellation for this native group.
    not_required,
    // The best-effort adapter cancel was dispatched. This is not terminal
    // proof; private sibling lifecycle confirmation is still required.
    requested,
    // The exact committed reservation was missing or REST cancellation was
    // ambiguous. Reconciliation has been latched without reviving stale legs.
    fatal,
};

// Engine-side enforcement of strategy-declared exit intents. Keyed by
// opener_order_id so two concurrent entries on the same (strategy,symbol)
// carry independent stops. Lifecycle: register_pending at entry submit ->
// on_fill promotes to armed (using actual fill px/qty) -> on_price returns
// every armed intent that triggers on this tick.
class ExitManager
{
public:
    ExitManager() = default;

    // Adds a pending intent for intent.opener_order_id. Calling again with
    // the same opener_id appends a second intent (TP1/TP2/SL scale-out).
    void register_pending(exit_intent intent);

    // Promotes all pending intents keyed by f.get_order_id() into armed,
    // using the fill price as entry reference and fill qty as the size.
    void on_fill(const fill_event& f);

    // Opener- or closer-aware variant. Passing the opener_order_id lets
    // the manager distinguish the two roles: when opener==fill.order_id
    // (the fill is the opener itself), pending intents are promoted to
    // armed; when opener != fill.order_id (this fill closes an earlier
    // entry), the armed bracket for that opener is dropped so it can't
    // fire again on a lot that's already been closed by signal.
    void on_fill(const fill_event& f, std::uint64_t opener_order_id);

    // Returns every armed intent at this symbol whose SL/TP/trailing/time
    // trigger crossed. Returned order_events carry opener_order_id so the
    // portfolio can close the correct lot.
    std::vector<order_event> on_price(const std::string& symbol,
                                      double px,
                                      std::chrono::system_clock::time_point ts);

    // Bar-aware variant: probes each armed intent against the bar's worst-
    // and best-case extremes (longs: low for SL, high for TP/trail; shorts:
    // inverted) so an intra-bar wick through the bracket fires it instead
    // of being missed when only the close is checked. Conservative ordering:
    // SL is evaluated before TP if both extremes crossed in the same bar,
    // and the trailing stop is tested at its pre-bar level (this bar's
    // favorable extreme raises the trail only for subsequent bars —
    // assuming the favorable extreme printed first would be look-ahead).
    // The returned order's price is the anchored fire price: the SL/TP
    // level itself, or the bar open when the bar gapped through it.
    std::vector<order_event> on_bar(const std::string& symbol,
                                    double open, double low, double high,
                                    double close,
                                    std::chrono::system_clock::time_point ts);

    // Drop everything for one opener (used when strategy exits via its own
    // signal path and the armed bracket should no longer fire).
    void cancel(std::uint64_t opener_order_id);

    // Legacy bulk-cancel: remove every pending/armed intent for a given
    // (strategy,symbol). Preserved for the net-flat notifier path; becomes
    // unused once strategies own their entry gating.
    void cancel(const std::string& strategy_name, const std::string& symbol);

    std::size_t armed_count() const { return armed_.size(); }
    std::size_t pending_count() const { return pending_.size(); }

    // Live opener count for a (strategy,symbol) pair across pending+armed.
    // Used by the engine to distinguish single-lot strategies (where a
    // net-flat transition can safely bulk-cancel any leftover bracket)
    // from multi-lot strategies (which own opener_order_id discipline).
    std::size_t openers_for(const std::string& strategy_name,
                            const std::string& symbol) const;

    // Optional venue-bracket integration. nullptr -> engine-side eval is
    // the only enforcement (current backtest/shadow behavior). Setting
    // an adapter activates defense-in-depth: the in-process armed intent
    // stays armed AND the venue gets resting orders. Set once at startup.
    void set_bracket_adapter(std::shared_ptr<IBracketAdapter> adapter)
    {
        bracket_adapter_ = std::move(adapter);
    }
    bool has_bracket_adapter() const
    {
        return static_cast<bool>(bracket_adapter_);
    }

    // Reverse-lookup for fills coming back from the venue. The engine
    // calls this on every inbound exec report - non-zero means the fill
    // is a venue-bracket leg and the engine should stamp opener_order_id
    // before routing through the closer-fill path. Thread-safe - may be
    // called from a provider's WS thread (e.g. user-data stream).
    std::uint64_t opener_for_exchange_order(const std::string& exchange_order_id) const;

    // Companion lookup so the engine can populate order_meta_ with the
    // right strategy_name when it synthesizes an order_id for a venue
    // bracket leg. Empty string = no match. Thread-safe.
    std::string strategy_name_for_exchange_order(const std::string& exchange_order_id) const;

    // Preflight-reserve a venue-native bracket-leg report on the engine
    // thread.  This accepts only an exact registered exchange id plus
    // immutable symbol, side, group and cumulative proof.  An economic result
    // is deliberately NOT committed here: call commit_native_bracket_economic
    // only after canonical fill accounting/audit succeeds, or roll it back on
    // an accounting exception. `not_native` is not an implicit acceptance;
    // engine code must turn it into its normal unknown-private-lifecycle
    // failure. No provider thread may call this method.
    native_bracket_resolution
    resolve_native_bracket_update(const native_bracket_update& update);

    // Complete or abandon one economic preflight reservation. These methods
    // require the exact `{source_sequence, leg_token}` returned by preflight,
    // are allocation-free with bounded state, and leave all committed economic
    // history untouched on rollback. A failed commit/rollback is a lifecycle
    // proof failure and callers must halt/reconcile.
    bool commit_native_bracket_economic(
        const native_bracket_economic_reservation& reservation) noexcept;
    bool rollback_native_bracket_economic(
        const native_bracket_economic_reservation& reservation) noexcept;

    // Call only after a successful commit of a terminal economic reservation.
    // For independent paired legs (no native OCO/list id), this dispatches the
    // adapter cancellation outside venue_mu_. A native OCO/list is already
    // atomic at the venue, so this deliberately does not issue a redundant
    // REST cancel; it waits for both child terminal reports and ALL_DONE.
    // An ambiguous adapter result preserves expected-sibling proof/deadline,
    // latches reconciliation, and never restores an active stale leg.
    native_bracket_sibling_cancel_result
    request_native_bracket_sibling_cancel(
        const native_bracket_economic_reservation& reservation) noexcept;

    // Resolves explicitly typed OCO/list status after the engine has already
    // established that it is a group lifecycle record.  A completed group is
    // never considered clean until the per-leg terminal proof is complete.
    native_bracket_group_resolution
    resolve_native_bracket_group_update(const native_bracket_group_update& update);

    // Native cancellation, OCO sibling confirmation, and final ALL_DONE
    // group proof use the same fixed, conservative deadline as ordinary
    // private cancellation proof. The injectable clock makes the 30-second
    // boundary deterministic in tests.
    static constexpr auto native_sibling_terminal_deadline = std::chrono::seconds{30};
    bool check_native_bracket_lifecycle_deadline(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    // Same-symbol venue admission gate.  Symbols are compared ASCII case-
    // insensitively because venue encoders canonicalize them differently from
    // callers.  A live, cancel-awaiting, sibling-awaiting, or poisoned native
    // leg blocks fresh admission until reconciliation resolves it.
    bool native_bracket_blocks_symbol_admission(std::string_view symbol) const;
    bool has_unresolved_native_bracket_lifecycle() const;
    bool native_bracket_requires_reconciliation() const;

    // Restart recovery: install an armed intent + venue handles for a
    // bracket the venue still has resting from a previous engine run.
    // Engine calls this at startup after the adapter enumerates open
    // brackets via IBracketAdapter::list_open(). The in-process armed
    // intent is what evaluate_exits checks against - the venue handles
    // are what cancel() forwards to. Both come back online together.
    void rehydrate(const IBracketAdapter::recovered_bracket& rb);

    // Phase A (MC object reuse): clears all pending/armed brackets and
    // venue state so the manager can be reused for the next trial.
    void reset();

    // Read-only snapshot of armed brackets for the live TUI. Designed
    // to be called from the engine's snapshot path (main thread). Each
    // row carries the in-process intent + whether the venue has the
    // bracket as a resting order (handles_ entry exists) so the panel
    // can show "engine-only" vs "venue-resting" state.
    struct armed_view
    {
        std::uint64_t opener_order_id = 0;
        std::string   strategy_name;
        std::string   symbol;
        order_side    close_side = order_side::sell;
        double        qty = 0.0;
        double        entry_price = 0.0;
        std::optional<double> stop_loss;
        std::optional<double> take_profit;
        bool          venue_managed = false;   // handles_ entry exists
        std::string   venue_list_id;           // empty if not OCO/grouped
        std::chrono::system_clock::time_point ts_armed{};
    };
    std::vector<armed_view> snapshot_armed() const;

private:
    struct armed_intent
    {
        exit_intent intent;
        double      entry_price = 0.0;
        double      best_price  = 0.0;  // running MFE for trailing
        std::chrono::system_clock::time_point ts_armed{};
    };

    using strategy_symbol_key = std::pair<std::string, std::string>;
    struct ss_hash
    {
        std::size_t operator()(const strategy_symbol_key& k) const noexcept
        {
            std::hash<std::string> h;
            return h(k.first) ^ (h(k.second) << 1);
        }
    };

    // Keyed by opener_order_id. Multimap so a single entry can carry
    // TP1/TP2/SL scale-outs.
    std::unordered_multimap<std::uint64_t, exit_intent>  pending_;
    std::unordered_multimap<std::uint64_t, armed_intent> armed_;
    std::unordered_map<std::uint64_t, double> opener_remaining_qty_;
    std::unordered_map<std::uint64_t, double> opener_close_in_flight_qty_;

    // Reverse index supporting legacy cancel(strategy,symbol).
    std::unordered_multimap<strategy_symbol_key, std::uint64_t, ss_hash>
        strategy_symbol_to_openers_;

    // Venue-bracket state. `exchange_to_leg_` retains a bounded immutable
    // proof record after a terminal report rather than erasing the reverse
    // identity at REST-cancel time.  That is what lets an OCO sibling race be
    // attributed and accounted before it is escalated to reconciliation.
    // Mutations are engine-thread-only; legacy read-only lookups may run on a
    // provider worker, so every access remains guarded by venue_mu_.
    static constexpr std::size_t native_symbol_capacity = 31;
    static constexpr std::size_t native_id_capacity = 95;
    static constexpr std::size_t native_strategy_capacity = 95;
    static constexpr std::size_t native_error_capacity = 127;
    static constexpr std::size_t native_execution_history_capacity = 32;
    static constexpr std::size_t native_lifecycle_history_capacity = 16;
    static constexpr std::size_t native_group_history_capacity = 16;
    static constexpr std::size_t native_retained_leg_capacity = 4096;
    static constexpr std::size_t native_retained_group_capacity = 2048;

    template <std::size_t Capacity>
    struct bounded_text
    {
        std::array<char, Capacity + 1> bytes{};
        std::uint8_t size = 0;

        [[nodiscard]] bool assign(std::string_view text) noexcept;
        [[nodiscard]] std::string_view view() const noexcept
        {
            return {bytes.data(), size};
        }
        [[nodiscard]] bool equals(std::string_view text) const noexcept
        {
            return view() == text;
        }
    };

    struct native_update_fingerprint
    {
        native_bracket_lifecycle lifecycle = native_bracket_lifecycle::ack;
        bounded_text<native_id_capacity> exchange_order_id;
        bounded_text<native_id_capacity> client_order_id;
        bounded_text<native_id_capacity> execution_id;
        bounded_text<native_symbol_capacity> symbol;
        bounded_text<native_id_capacity> group_id;
        bounded_text<15> commission_asset;
        bounded_text<native_error_capacity> error;
        order_side side = order_side::buy;
        std::int64_t event_time_ms = 0;
        double last_fill_qty = 0.0;
        double last_fill_price = 0.0;
        double cumulative_qty = 0.0;
        double commission = 0.0;
        bool cumulative_reported = false;
        bool lifecycle_only = false;

        [[nodiscard]] bool assign(const native_bracket_update& update) noexcept;
        [[nodiscard]] bool equals(const native_bracket_update& update) const noexcept;
    };

    enum class native_leg_lifecycle : std::uint8_t
    {
        active,
        cancel_requested,
        expected_sibling_terminal,
        terminal_filled,
        terminal_nonfill,
        reconciliation_required,
    };

    struct native_venue_leg
    {
        std::uint64_t opener_order_id = 0;
        // Generated once at registration and never reused while retained.
        // It is the leg half of an economic reservation token.
        std::uint64_t reservation_leg_token = 0;
        bounded_text<native_strategy_capacity> strategy_name;
        bounded_text<native_symbol_capacity> symbol;
        bounded_text<native_id_capacity> exchange_order_id;
        bounded_text<native_id_capacity> group_id;
        native_bracket_leg_role role = native_bracket_leg_role::stop_loss;
        order_side close_side = order_side::sell;
        double placed_qty = 0.0;
        double cumulative_qty = 0.0;
        native_leg_lifecycle lifecycle = native_leg_lifecycle::active;
        std::chrono::steady_clock::time_point terminal_deadline{};
        std::array<native_update_fingerprint, native_execution_history_capacity>
            execution_history{};
        std::uint8_t execution_history_size = 0;
        std::array<native_update_fingerprint, native_lifecycle_history_capacity>
            lifecycle_history{};
        std::uint8_t lifecycle_history_size = 0;
        // Set only by a successfully committed terminal economic fill. An
        // independent pair uses this to prove that the subsequent adapter
        // cancellation request belongs to that exact economic fact.
        std::uint64_t committed_terminal_source_sequence = 0;
        bool sibling_cancel_required = false;
        bool sibling_cancel_dispatched = false;
        struct pending_economic_reservation
        {
            bool active = false;
            std::uint64_t source_sequence = 0;
            native_update_fingerprint fingerprint;
            double next_cumulative_qty = 0.0;
            bool terminal = false;
            bool requires_reconciliation = false;
        } pending_economic{};
    };

    enum class native_group_lifecycle : std::uint8_t
    {
        active,
        awaiting_sibling_terminal,
        completed,
        reconciliation_required,
    };

    struct native_group_fingerprint
    {
        native_bracket_group_status status = native_bracket_group_status::active;
        bounded_text<native_id_capacity> group_id;
        bounded_text<native_symbol_capacity> symbol;
        std::int64_t event_time_ms = 0;

        [[nodiscard]] bool assign(const native_bracket_group_update& update) noexcept;
        [[nodiscard]] bool equals(const native_bracket_group_update& update) const noexcept;
    };

    struct native_oco_group
    {
        std::uint64_t opener_order_id = 0;
        bounded_text<native_id_capacity> group_id;
        bounded_text<native_symbol_capacity> symbol;
        bounded_text<native_id_capacity> stop_exchange_id;
        bounded_text<native_id_capacity> take_profit_exchange_id;
        bounded_text<native_id_capacity> winning_exchange_id;
        bounded_text<native_id_capacity> expected_sibling_exchange_id;
        native_group_lifecycle lifecycle = native_group_lifecycle::active;
        bool completed_status_seen = false;
        std::chrono::steady_clock::time_point sibling_terminal_deadline{};
        std::array<native_group_fingerprint, native_group_history_capacity>
            history{};
        std::uint8_t history_size = 0;
    };

    struct native_string_hash
    {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept
        {
            return std::hash<std::string_view>{}(value);
        }
        [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept
        {
            return (*this)(std::string_view{value});
        }
    };

    struct native_string_equal
    {
        using is_transparent = void;

        [[nodiscard]] bool operator()(std::string_view left,
                                      std::string_view right) const noexcept
        {
            return left == right;
        }
        [[nodiscard]] bool operator()(const std::string& left,
                                      const std::string& right) const noexcept
        {
            return std::string_view{left} == std::string_view{right};
        }
        [[nodiscard]] bool operator()(const std::string& left,
                                      std::string_view right) const noexcept
        {
            return std::string_view{left} == right;
        }
        [[nodiscard]] bool operator()(std::string_view left,
                                      const std::string& right) const noexcept
        {
            return left == std::string_view{right};
        }
    };

    std::shared_ptr<IBracketAdapter> bracket_adapter_;
    std::unordered_map<std::uint64_t, bracket_handles> handles_;
    std::unordered_map<std::string, native_venue_leg,
                       native_string_hash, native_string_equal> exchange_to_leg_;
    std::unordered_map<std::string, native_oco_group,
                       native_string_hash, native_string_equal> native_groups_;
    std::unordered_set<std::uint64_t>                  venue_cancel_requested_openers_;
    std::uint64_t native_next_leg_token_ = 1;
    bool native_reconciliation_required_ = false;
    mutable std::mutex venue_mu_;

    void untrack_opener(std::uint64_t opener_order_id,
                        const std::string& strategy_name,
                        const std::string& symbol);

    double consume_opener_qty(std::uint64_t opener_order_id, double requested_qty);

    // Single point that requests venue-side cleanup. Native reverse identities
    // intentionally survive the REST request until private terminal proof.
    // Called from every cancel path so adapter.cancel() is invoked exactly
    // once per opener while that proof remains outstanding.
    void release_venue_bracket(std::uint64_t opener_order_id);

    static bool same_number(double left, double right) noexcept;
    static bool ascii_case_equal(std::string_view left, std::string_view right) noexcept;
    static bool is_valid_native_side(order_side side) noexcept;
    static bool is_native_leg_terminal(native_leg_lifecycle lifecycle) noexcept;
    static bool is_native_leg_unresolved(native_leg_lifecycle lifecycle) noexcept;
    static bool valid_native_update_shape(const native_bracket_update& update) noexcept;

    bool install_native_venue_state_locked(std::uint64_t opener_order_id,
                                           std::string_view strategy_name,
                                           std::string_view symbol,
                                           order_side close_side,
                                           double placed_qty,
                                           const bracket_handles& handles);
    bool install_native_leg_locked(std::uint64_t opener_order_id,
                                   std::string_view strategy_name,
                                   std::string_view symbol,
                                   order_side close_side,
                                   double placed_qty,
                                   std::string_view exchange_order_id,
                                   std::string_view group_id,
                                   native_bracket_leg_role role);
    bool install_native_group_locked(std::uint64_t opener_order_id,
                                     std::string_view symbol,
                                     std::string_view group_id,
                                     std::string_view stop_exchange_id,
                                     std::string_view take_profit_exchange_id);
    void mark_native_cancel_requested_locked(std::uint64_t opener_order_id,
                                             std::chrono::steady_clock::time_point now);
    void mark_native_reconciliation_required_locked(native_venue_leg& leg);
    void mark_native_group_reconciliation_required_locked(native_oco_group& group);
    void mark_native_leg_filled_locked(native_venue_leg& leg,
                                       std::chrono::steady_clock::time_point now,
                                       bool& sibling_economic_race,
                                       bool& sibling_cancel_required);
    void maybe_complete_native_group_locked(native_oco_group& group);
    native_venue_leg* find_native_sibling_locked(
        const native_venue_leg& leg) noexcept;
    native_venue_leg* find_native_leg_for_reservation_locked(
        const native_bracket_economic_reservation& reservation) noexcept;
    bool has_native_legs_for_opener_locked(std::uint64_t opener_order_id) const;
    bool all_native_legs_terminal_for_opener_locked(std::uint64_t opener_order_id) const;
    bool native_group_blocks_symbol_admission_locked(std::string_view symbol) const;
};

template <std::size_t Capacity>
bool ExitManager::bounded_text<Capacity>::assign(std::string_view text) noexcept
{
    if (text.size() > Capacity) return false;
    std::copy(text.begin(), text.end(), bytes.begin());
    bytes[text.size()] = '\0';
    size = static_cast<std::uint8_t>(text.size());
    return true;
}

}
