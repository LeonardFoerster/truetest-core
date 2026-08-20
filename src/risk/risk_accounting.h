#pragma once

// ============================================================
// R3 — authoritative risk-view construction.
//
// Turns the three authoritative sources
//   * portfolio        — signed position quantity + cost basis (fills only)
//   * OrderTracker     — open orders, remaining quantity, per-symbol pending
//   * mark_point       — last price + observation timestamp
// into the risk_snapshot views RiskManager enforces against.
//
// Deliberately header-only and templated on the mark lookup: this runs on the
// order hot path, so no virtual dispatch and no allocation. Cost is
// O(#distinct symbols) — bounded by SymbolTable::kMaxSymbols and typically
// 1-5 — never O(#orders ever seen).
//
// See docs/internal/r3-authoritative-risk-accounting.md.
// ============================================================

#include "risk_manager.h"

#include "../execution/mark_point.h"
#include "../execution/order_tracker.h"
#include "../execution/portfolio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace truetest::risk {

inline constexpr double kQtyEpsilon = 1e-12;

// A mark is stale when it is older than the configured budget. max_age_ms <= 0
// disables the classification (marks are then valid or missing only), which is
// the default for offline backtests where the sim clock jumps by whole bars.
[[nodiscard]] inline mark_quality classify_mark(
    const mark_point& mark,
    std::chrono::system_clock::time_point now,
    std::int64_t max_age_ms) noexcept
{
    if (!mark.usable())
        return mark_quality::missing;
    if (max_age_ms <= 0)
        return mark_quality::valid;
    const auto age = mark_age_ms(mark, now);
    if (age < 0)
        return mark_quality::valid;   // no observation clock: cannot claim stale
    return (age > max_age_ms) ? mark_quality::stale : mark_quality::valid;
}

// Accumulator for one instrument. Kept separate so the per-symbol and the
// portfolio pass share exactly one definition of "exposure".
struct instrument_inputs
{
    double position_qty = 0.0;
    double cost_basis = 0.0;
    symbol_open_exposure open{};
    mark_point mark{};
    mark_quality quality = mark_quality::missing;
    std::int64_t age_ms = -1;
    bool exposure_tracked = false;
};

[[nodiscard]] inline instrument_risk_view make_instrument_view(
    const instrument_inputs& in) noexcept
{
    instrument_risk_view view;
    view.position_qty = in.position_qty;
    view.mark_state = in.quality;
    view.mark_price = (in.quality == mark_quality::missing) ? 0.0 : in.mark.price;
    view.mark_age_ms = in.age_ms;
    view.exposure_tracked = in.exposure_tracked;

    const double mark = view.mark_price;
    view.position_notional = std::abs(in.position_qty) * mark;

    view.open_buy_qty = in.open.open_buy_qty;
    view.open_sell_qty = in.open.open_sell_qty;
    view.open_order_count = in.open.open_order_count;
    view.open_buy_notional = in.open.open_buy_qty * mark;
    view.open_sell_notional = in.open.open_sell_qty * mark;

    // Signed, same convention as position_qty: the position that would exist
    // if every live order on that side filled in full.
    view.worst_case_long_qty = in.position_qty + in.open.open_buy_qty;
    view.worst_case_short_qty = in.position_qty - in.open.open_sell_qty;
    view.worst_case_long_notional = std::abs(view.worst_case_long_qty) * mark;
    view.worst_case_short_notional = std::abs(view.worst_case_short_qty) * mark;

    // Mark-to-market against the entry basis. cost_basis is P&L accounting
    // only — it is never used as a current exposure.
    view.unrealized_pnl = in.position_qty * mark - in.cost_basis;
    return view;
}

// Fill the authoritative views on `snap` for `candidate_symbol`.
//
// mark_for: callable `mark_point(const std::string&)`; return a default
// constructed mark_point for "no mark known".
// now: the simulation/event clock (never wall clock — determinism).
template <typename MarkFn>
void build_risk_view(risk_snapshot& snap,
                     const std::string& candidate_symbol,
                     const portfolio& port,
                     const OrderTracker& ledger,
                     std::chrono::system_clock::time_point now,
                     std::int64_t max_mark_age_ms,
                     MarkFn&& mark_for)
{
    snap.ledger_authoritative = true;
    snap.instrument = {};
    snap.portfolio = {};

    auto& pv = snap.portfolio;
    pv.open_order_count = ledger.active_count();

    const auto& positions = port.get_positions();
    double equity = port.get_cash();
    bool equity_complete = true;

    const auto accumulate = [&](const std::string& symbol,
                                const symbol_open_exposure& open,
                                bool exposure_tracked)
    {
        instrument_inputs in;
        in.open = open;
        in.exposure_tracked = exposure_tracked;
        if (auto it = positions.find(symbol); it != positions.end())
        {
            in.position_qty = it->second.qty;
            in.cost_basis = it->second.cost_basis;
        }
        in.mark = mark_for(symbol);
        in.quality = classify_mark(in.mark, now, max_mark_age_ms);
        in.age_ms = mark_age_ms(in.mark, now);

        const bool holds_position = std::abs(in.position_qty) > kQtyEpsilon;
        const bool holds_pending =
            open.open_buy_qty > kQtyEpsilon || open.open_sell_qty > kQtyEpsilon;

        if (in.quality == mark_quality::stale)
            ++pv.stale_marks;
        if ((holds_position || holds_pending) && in.quality == mark_quality::missing)
        {
            ++pv.positions_without_usable_mark;
            if (holds_position)
                equity_complete = false;   // a partial equity is not an equity
        }

        const auto view = make_instrument_view(in);

        pv.gross_exposure += view.position_notional;
        pv.net_exposure += in.position_qty * view.mark_price;
        pv.unrealized_pnl += view.unrealized_pnl;
        pv.worst_case_gross_exposure +=
            std::max(view.worst_case_long_notional, view.worst_case_short_notional);
        equity += in.position_qty * view.mark_price;

        if (symbol == candidate_symbol)
            snap.instrument = view;
    };

    // Pass 1: every symbol the order ledger knows (covers symbols that have
    // open orders but no position yet).
    ledger.for_each_symbol_exposure(
        [&](const std::string& symbol, const symbol_open_exposure& open) {
            accumulate(symbol, open, true);
        });

    // Pass 2: positions the ledger has never seen (checkpoint restore, direct
    // portfolio seeding in tests).
    for (const auto& [symbol, pos] : positions)
    {
        if (ledger.tracks_symbol(symbol))
            continue;
        (void)pos;
        accumulate(symbol, symbol_open_exposure{}, !ledger.symbol_capacity_exhausted());
    }

    // The candidate symbol may be entirely new (no position, no prior order).
    // It still needs a view so the mark quality of the instrument being traded
    // is never silently absent.
    if (!ledger.tracks_symbol(candidate_symbol)
        && positions.find(candidate_symbol) == positions.end())
    {
        instrument_inputs in;
        in.exposure_tracked = !ledger.symbol_capacity_exhausted();
        in.mark = mark_for(candidate_symbol);
        in.quality = classify_mark(in.mark, now, max_mark_age_ms);
        in.age_ms = mark_age_ms(in.mark, now);
        if (in.quality == mark_quality::stale)
            ++pv.stale_marks;
        snap.instrument = make_instrument_view(in);
    }

    // An unmarkable position makes the account valuation unknowable. Report
    // NaN rather than a plausible-looking partial number: RiskManager's
    // percentage-of-equity caps fail closed on a non-finite equity, which is
    // the intended behaviour (unchanged from marked_account_equity()).
    pv.equity = equity_complete
        ? equity : std::numeric_limits<double>::quiet_NaN();
    // Realized = everything that is not open mark-to-market P&L. Funding
    // settlements are already inside cash and therefore inside equity.
    pv.realized_pnl = equity_complete
        ? (equity - port.get_initial_balance()) - pv.unrealized_pnl
        : std::numeric_limits<double>::quiet_NaN();
    snap.equity = pv.equity;
}

} // namespace truetest::risk
