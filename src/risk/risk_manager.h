#pragma once

// ============================================================
// LIVE-SAFETY SURFACE — Phase 1 freeze (see prod.md)
// Any edit requires explicit two-person CCB review + 4 h
// mainnet shadow run on engine_shadow before merge.
// Authoritative path list: scripts/check-live-safety-freeze.sh
// ============================================================
//
// R3 (authoritative risk accounting): every quantity this file reasons about
// comes from the authoritative ledger (OrderTracker), the fill-driven
// portfolio, or a timestamped mark — never from analytics/performance
// counters and never from cost basis. See
// docs/internal/r3-authoritative-risk-accounting.md.

#include "../core/event.h"
#include "../execution/portfolio.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>

// Forward-decl breaks a circular include with analytics.h.
struct AnalyticsReport;

struct risk_limits
{
    double max_position_value = 1e9;
    double max_drawdown = 0.30;
    double max_loss_per_trade = 10000.0;
    int max_open_orders = 1000;
    double max_portfolio_exposure = 5e9;

    double max_daily_loss = 0.0;  // daily_loss_ accumulates realized negative PnL
    int daily_reset_hour = 0;
    int max_trades_per_hour = 0;
    int max_orders_per_minute = 0;

    // Phase 2.3 — position sizing as % of equity + volatility
    double max_position_pct_of_equity = 0.0;  // 0 = disabled

    // Phase 2.4 — circuit breakers
    double max_spread_bps = 0.0;         // 0 = disabled
    double max_funding_8h_rate = 0.0;    // 0 = disabled (as fraction, e.g. 0.0005 = 0.05%)

    // R3 — hard inventory limit in base quantity, applied to the worst case
    // (current position + every open order on the increasing side + this
    // candidate). 0 = disabled. Inventory-reducing orders stay permitted
    // after a breach so an operator can always de-risk.
    double max_symbol_inventory_qty = 0.0;

    // R3 — mark-data quality. max_mark_age_ms classifies a mark as stale
    // (0 = staleness classification disabled). require_fresh_mark makes a
    // stale or missing mark refuse inventory-increasing orders instead of
    // falling back to the candidate's own limit price.
    std::int64_t max_mark_age_ms = 0;
    bool require_fresh_mark = false;

    // NOTE (R3): `max_portfolio_var_estimate` was removed. It advertised a
    // portfolio-VaR bound the repository never computed or enforced — one
    // declaration, zero readers. See the R3 design note §6 before adding any
    // replacement: a VaR limit lands together with an actual estimator.
};

enum class risk_action { pass, reject, halt, unwind };

// Stable, machine-readable identity of the rule that produced a non-pass
// decision. Fed into the existing audit-sink reason-code surface
// (record_rejection / record_event "risk_decision") so a reject or halt is
// attributable without parsing prose.
enum class risk_rule : std::uint8_t
{
    none,
    max_open_orders,
    position_limit,
    hard_inventory_limit,
    portfolio_exposure,
    position_pct_of_equity,
    invalid_equity,
    stale_mark,
    drawdown,
    daily_loss,
    loss_per_trade,
    trades_per_hour,
    orders_per_minute,
    spread_limit,
    funding_limit
};

[[nodiscard]] inline const char* to_string(risk_rule rule) noexcept
{
    switch (rule)
    {
    case risk_rule::none:                   return "none";
    case risk_rule::max_open_orders:        return "max_open_orders";
    case risk_rule::position_limit:         return "position_limit";
    case risk_rule::hard_inventory_limit:   return "hard_inventory_limit";
    case risk_rule::portfolio_exposure:     return "portfolio_exposure";
    case risk_rule::position_pct_of_equity: return "position_pct_of_equity";
    case risk_rule::invalid_equity:         return "invalid_equity";
    case risk_rule::stale_mark:             return "stale_mark";
    case risk_rule::drawdown:               return "drawdown";
    case risk_rule::daily_loss:             return "daily_loss";
    case risk_rule::loss_per_trade:         return "loss_per_trade";
    case risk_rule::trades_per_hour:        return "trades_per_hour";
    case risk_rule::orders_per_minute:      return "orders_per_minute";
    case risk_rule::spread_limit:           return "spread_limit";
    case risk_rule::funding_limit:          return "funding_limit";
    }
    return "none";
}

// Quality of the mark backing a mark-to-market valuation.
enum class mark_quality : std::uint8_t { missing, stale, valid };

[[nodiscard]] inline const char* to_string(mark_quality quality) noexcept
{
    switch (quality)
    {
    case mark_quality::missing: return "missing";
    case mark_quality::stale:   return "stale";
    case mark_quality::valid:   return "valid";
    }
    return "missing";
}

// How a candidate order moves inventory relative to the current position.
enum class inventory_effect : std::uint8_t { neutral, increasing, reducing };

[[nodiscard]] inline const char* to_string(inventory_effect effect) noexcept
{
    switch (effect)
    {
    case inventory_effect::neutral:    return "neutral";
    case inventory_effect::increasing: return "increasing";
    case inventory_effect::reducing:   return "reducing";
    }
    return "neutral";
}

// Classify a candidate order against a signed position quantity.
// A flip (opposite side, larger than the position) opens fresh opposite-side
// exposure and is therefore increasing, not reducing.
[[nodiscard]] inline inventory_effect classify_inventory_effect(
    order_side side, double order_qty, double position_qty) noexcept
{
    constexpr double eps = 1e-12;
    if (!(order_qty > eps))
        return inventory_effect::neutral;
    if (!(std::abs(position_qty) > eps))
        return inventory_effect::increasing;   // opening from flat
    const bool opposes = (position_qty > 0.0 && side == order_side::sell)
                      || (position_qty < 0.0 && side == order_side::buy);
    if (!opposes)
        return inventory_effect::increasing;
    return (order_qty <= std::abs(position_qty) + eps)
        ? inventory_effect::reducing
        : inventory_effect::increasing;
}

// ---------------------------------------------------------------------------
// Authoritative per-instrument risk view (R3).
//
// Built from the position ledger + the open-order ledger + a timestamped
// mark. Every notional here is mark-to-market; cost basis is P&L accounting
// and never appears as a current exposure.
// ---------------------------------------------------------------------------
struct instrument_risk_view
{
    double position_qty = 0.0;              // signed, authoritative (fills only)
    double mark_price = 0.0;
    mark_quality mark_state = mark_quality::missing;
    std::int64_t mark_age_ms = -1;          // -1 = unknown

    double position_notional = 0.0;         // |position_qty| * mark_price

    double open_buy_qty = 0.0;              // Σ remaining qty of open buys
    double open_sell_qty = 0.0;
    double open_buy_notional = 0.0;
    double open_sell_notional = 0.0;
    std::size_t open_order_count = 0;

    // Position if every live order on that side filled in full. Signed, same
    // convention as position_qty. These are what makes a hard limit hold
    // under pending orders instead of only after fills land.
    double worst_case_long_qty = 0.0;       // position + Σ open buy remaining
    double worst_case_short_qty = 0.0;      // position - Σ open sell remaining
    double worst_case_long_notional = 0.0;
    double worst_case_short_notional = 0.0;

    double unrealized_pnl = 0.0;            // qty*mark - cost_basis

    // False when the ledger could not aggregate this symbol (symbol-table
    // capacity exhausted). Per-symbol limits must fail closed, not read zero.
    bool exposure_tracked = false;
};

// Portfolio-level aggregate over every instrument the portfolio holds.
struct portfolio_risk_view
{
    double gross_exposure = 0.0;            // Σ |qty| * mark
    double net_exposure = 0.0;              // Σ  qty  * mark
    double worst_case_gross_exposure = 0.0; // Σ max(|wc_long|, |wc_short|) * mark
    std::size_t open_order_count = 0;       // authoritative ledger count

    double realized_pnl = 0.0;
    double unrealized_pnl = 0.0;
    // Realized loss accumulated since the daily reset boundary, mirrored from
    // RiskManager::daily_realized_loss() by the snapshot's owner. Positive
    // means "lost this much today"; the max_daily_loss limit is enforced from
    // the same accumulator.
    double daily_realized_loss = 0.0;
    double equity = 0.0;                    // cash + Σ qty*mark (NaN if unmarkable)

    std::size_t positions_without_usable_mark = 0;
    std::size_t stale_marks = 0;
};

// POD subset of AnalyticsReport — building a full report per order
// dominates the hot path (Sharpe/Sortino, vectors), so carry only what
// the risk checks actually read, plus the R3 authoritative views.
struct risk_snapshot
{
    double       max_drawdown   = 0.0;  // percent, matches AnalyticsReport
    double       last_trade_pnl = 0.0;
    bool         has_last_trade = false;
    std::size_t  last_trade_seq = 0;

    // Note: daily_loss is maintained internally in RiskManager (realized negative trade PnL)

    // Phase 2.3 — filled by analytics worker before passing to RiskManager
    double equity = 0.0;
    double realized_vol_1h = 0.0;   // simple Welford or EWMA proxy

    // Phase 2.4 — current market conditions for circuit breakers
    double current_spread_bps = 0.0;
    double current_funding_8h_rate = 0.0;  // last known 8h funding rate (fraction)
    // R3: an unknown funding rate must not read as "0.0, therefore inside the
    // limit". The funding breaker only engages when a rate actually exists.
    bool   funding_rate_known = false;

    // R3 — authoritative views. ledger_authoritative is false for consumers
    // that hold no order ledger (the observational risk workers); every
    // pending-order-inclusive check is skipped for those rather than reading
    // a structurally-zero exposure as "flat".
    bool ledger_authoritative = false;
    instrument_risk_view instrument{};
    portfolio_risk_view  portfolio{};
};

class RiskManager
{
public:
    explicit RiskManager(risk_limits limits = {});

    bool open_order_limit_reached(std::size_t open_order_count) const;

    risk_action check_order(const order_event& order,
                            const portfolio& port,
                            const risk_snapshot& snap,
                            std::size_t open_order_count = 0,
                            risk_rule* rule_out = nullptr);

    risk_action check_post_fill(const fill_event& fill,
                                const portfolio& port,
                                const risk_snapshot& snap,
                                risk_rule* rule_out = nullptr);

    // Legacy: workers/tests that already hold an AnalyticsReport.
    risk_action check_order(const order_event& order,
                            const portfolio& port,
                            const AnalyticsReport& snap,
                            std::size_t open_order_count = 0,
                            risk_rule* rule_out = nullptr);

    risk_action check_post_fill(const fill_event& fill,
                                const portfolio& port,
                                const AnalyticsReport& snap,
                                risk_rule* rule_out = nullptr);

    void on_fill(const fill_event& fill);

    const risk_limits& limits() const noexcept { return limits_; }

    // Realized loss accumulated since the last daily reset. Exposed so the
    // risk snapshot and the operator dashboards report the same number the
    // max_daily_loss limit is enforced from (both used to show a hardcoded 0).
    double daily_realized_loss() const noexcept { return daily_loss_; }

    // Phase A (MC object reuse)
    void reset();

private:
    risk_limits limits_;

    struct timestamped_entry {
        std::chrono::system_clock::time_point ts;
    };
    std::deque<timestamped_entry> order_timestamps_;
    std::deque<timestamped_entry> trade_timestamps_;

    double daily_loss_ = 0.0;  // realized losses (see check_post_fill)
    double daily_start_equity_ = 0.0;
    std::chrono::system_clock::time_point daily_reset_tp_{};

    // Guard to avoid re-adding the same closed trade if check_post_fill is
    // invoked multiple times for the latest analytics snapshot.
    std::size_t last_daily_trade_seq_added_ = 0;

    void update_daily_reset(std::chrono::system_clock::time_point now);
    void prune_old_entries(std::deque<timestamped_entry>& entries,
                           std::chrono::system_clock::time_point cutoff);
};
