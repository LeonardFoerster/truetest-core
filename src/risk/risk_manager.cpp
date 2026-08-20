#include "risk_manager.h"
#include "../analytics/analytics.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kQtyEps = 1e-12;

// R3: there is deliberately no cost-basis-derived price helper any more.
// Cost basis is P&L accounting; current exposure is mark-to-market only.

struct valuation
{
    double price = 0.0;
    bool   usable = false;
    bool   degraded = false;   // fell back to the candidate's own limit price
};

// Resolve the price used to value current and worst-case exposure.
// Precedence: fresh mark > stale mark (unless fresh marks are required) >
// the candidate order's own limit price. Never cost basis.
valuation resolve_valuation(const order_event& order,
                            const instrument_risk_view& view,
                            bool require_fresh_mark)
{
    valuation out;
    if (view.mark_state == mark_quality::valid
        && std::isfinite(view.mark_price) && view.mark_price > 0.0)
    {
        out.price = view.mark_price;
        out.usable = true;
        return out;
    }
    if (require_fresh_mark)
        return out;   // stale/missing mark must not be laundered into a price

    if (view.mark_state == mark_quality::stale
        && std::isfinite(view.mark_price) && view.mark_price > 0.0)
    {
        out.price = view.mark_price;
        out.usable = true;
        out.degraded = true;
        return out;
    }
    const double order_price = order.get_price();
    if (std::isfinite(order_price) && order_price > 0.0)
    {
        out.price = order_price;
        out.usable = true;
        out.degraded = true;
    }
    return out;
}

} // namespace

RiskManager::RiskManager(risk_limits limits)
    : limits_(std::move(limits)) {}

bool RiskManager::open_order_limit_reached(std::size_t open_order_count) const
{
    return limits_.max_open_orders > 0 &&
           open_order_count >= static_cast<std::size_t>(limits_.max_open_orders);
}

void RiskManager::prune_old_entries(std::deque<timestamped_entry>& entries,
                                    std::chrono::system_clock::time_point cutoff)
{
    while (!entries.empty() && entries.front().ts < cutoff)
        entries.pop_front();
}

void RiskManager::update_daily_reset(std::chrono::system_clock::time_point now)
{
    if (daily_reset_tp_ == std::chrono::system_clock::time_point{})
    {
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_r(&tt, &utc);
        utc.tm_hour = limits_.daily_reset_hour;
        utc.tm_min = 0;
        utc.tm_sec = 0;
        auto boundary = std::chrono::system_clock::from_time_t(timegm(&utc));
        if (boundary <= now)
            boundary += std::chrono::hours(24);
        daily_reset_tp_ = boundary;
    }

    if (now >= daily_reset_tp_)
    {
        daily_loss_ = 0.0;
        daily_start_equity_ = 0.0;
        while (daily_reset_tp_ <= now)
            daily_reset_tp_ += std::chrono::hours(24);
    }
}

risk_action RiskManager::check_order(const order_event& order,
                                     const portfolio& port,
                                     const risk_snapshot& snap,
                                     std::size_t open_order_count,
                                     risk_rule* rule_out)
{
    const auto deny = [&](risk_rule rule, risk_action action) {
        if (rule_out) *rule_out = rule;
        return action;
    };
    if (rule_out) *rule_out = risk_rule::none;

    const auto& positions = port.get_positions();
    const auto it = positions.find(order.get_symbol());
    // Authoritative signed position quantity — produced only by fills.
    const double position_qty = (it != positions.end()) ? it->second.qty : 0.0;

    const auto effect = classify_inventory_effect(
        order.get_side(), order.get_quantity(), position_qty);
    const bool reducing_exposure = (effect == inventory_effect::reducing);

    // ---- worst-case inventory including already-open orders ---------------
    // Pending orders are real future exposure: a candidate buy must be judged
    // against current position + every open buy + itself, not against the
    // position alone. Consumers without an order ledger (the observational
    // risk workers) contribute zero pending quantity and are flagged so that
    // structural absence never masquerades as "no open orders".
    const auto& view = snap.instrument;
    const double open_buy_qty  = snap.ledger_authoritative ? view.open_buy_qty  : 0.0;
    const double open_sell_qty = snap.ledger_authoritative ? view.open_sell_qty : 0.0;
    const double candidate_qty = std::max(0.0, order.get_quantity());
    const double candidate_buy =
        (order.get_side() == order_side::buy) ? candidate_qty : 0.0;
    const double candidate_sell =
        (order.get_side() == order_side::sell) ? candidate_qty : 0.0;

    const double worst_case_long_qty  = position_qty + open_buy_qty + candidate_buy;
    const double worst_case_short_qty = position_qty - open_sell_qty - candidate_sell;
    const double worst_case_abs_qty =
        std::max(std::abs(worst_case_long_qty), std::abs(worst_case_short_qty));

    // Portfolio DD: block new risk only. Reduce-only / exit orders must still
    // pass so inventory can be flattened after a breach (soft backtest and
    // live halt paths both need this — otherwise exits are rejected too).
    if (!reducing_exposure &&
        snap.max_drawdown / 100.0 >= limits_.max_drawdown)
        return deny(risk_rule::drawdown, risk_action::halt);

    // The engine-owned OrderTracker ledger supplies the exact lifecycle count
    // before this candidate becomes active. Analytics counters are reporting
    // only and are structurally absent from risk_snapshot since R3.
    // This capacity limit deliberately has no reduce-only exemption.
    if (open_order_limit_reached(open_order_count))
        return deny(risk_rule::max_open_orders, risk_action::reject);

    // ---- hard inventory limit (quantity, no mark required) ----------------
    // Applies to the worst case, so a stack of resting buys cannot walk the
    // book past the limit one acknowledged order at a time. Risk-reducing
    // orders stay permitted after a breach.
    if (limits_.max_symbol_inventory_qty > 0.0 && !reducing_exposure)
    {
        if (!view.exposure_tracked && snap.ledger_authoritative)
        {
            // Ledger could not aggregate this symbol: fail closed rather than
            // read an untracked symbol as flat.
            return deny(risk_rule::hard_inventory_limit, risk_action::reject);
        }
        if (worst_case_abs_qty > limits_.max_symbol_inventory_qty + kQtyEps)
            return deny(risk_rule::hard_inventory_limit, risk_action::reject);
    }

    // ---- mark-to-market valuation ----------------------------------------
    const bool needs_valuation =
        limits_.max_position_value > 0.0 ||
        limits_.max_portfolio_exposure > 0.0 ||
        limits_.max_position_pct_of_equity > 0.0;

    const auto val = resolve_valuation(order, view, limits_.require_fresh_mark);

    // A stale or missing mark must never quietly produce a "valid" risk
    // number. Inventory-increasing orders are refused; reductions stay
    // possible so bad market data cannot trap inventory.
    if (!reducing_exposure && !val.usable &&
        (needs_valuation || limits_.require_fresh_mark))
        return deny(risk_rule::stale_mark, risk_action::reject);

    const double mark = val.price;
    const double worst_case_notional = worst_case_abs_qty * mark;

    if (!reducing_exposure &&
        limits_.max_position_value > 0.0 &&
        worst_case_notional > limits_.max_position_value)
        return deny(risk_rule::position_limit, risk_action::reject);

    // Percentage-of-equity caps are meaningful only with a finite positive
    // account valuation. A missing mark must not turn a configured cap off.
    // True reductions remain exempt so a broken mark cannot trap inventory.
    if (!reducing_exposure &&
        limits_.max_position_pct_of_equity > 0.0 &&
        (!std::isfinite(snap.equity) || snap.equity <= 0.0))
        return deny(risk_rule::invalid_equity, risk_action::reject);

    // Phase 2.3 - max position as % of equity
    if (!reducing_exposure &&
        limits_.max_position_pct_of_equity > 0.0) {
        double max_notional = snap.equity * limits_.max_position_pct_of_equity;
        if (worst_case_notional > max_notional)
            return deny(risk_rule::position_pct_of_equity, risk_action::reject);
    }

    // ---- portfolio-level aggregation --------------------------------------
    // Mark-to-market gross exposure across every instrument. When the caller
    // supplied an authoritative portfolio view, the candidate's symbol is
    // swapped for its worst case; otherwise the remaining symbols are valued
    // from this order's mark as a single-symbol fallback (cost basis is never
    // used as an exposure proxy).
    double projected_total_exposure = worst_case_notional;
    if (snap.ledger_authoritative)
    {
        projected_total_exposure +=
            std::max(0.0, snap.portfolio.gross_exposure - view.position_notional);
    }
    else if (limits_.max_portfolio_exposure > 0.0 && !reducing_exposure)
    {
        // No authoritative portfolio view: the candidate's own mark is the
        // only price available, so any *other* held instrument is genuinely
        // unvaluable here. Refuse to increase inventory rather than value a
        // foreign symbol at this order's price (or, as before R3, at its cost
        // basis). Reductions stay exempt.
        for (const auto& [sym, pos] : positions)
        {
            if (sym == order.get_symbol() || std::abs(pos.qty) <= kQtyEps)
                continue;
            return deny(risk_rule::stale_mark, risk_action::reject);
        }
    }

    if (!reducing_exposure &&
        limits_.max_portfolio_exposure > 0.0 &&
        projected_total_exposure > limits_.max_portfolio_exposure)
        return deny(risk_rule::portfolio_exposure, risk_action::reject);

    // Phase 2.3 - portfolio-wide % of equity
    if (!reducing_exposure &&
        limits_.max_position_pct_of_equity > 0.0) {
        double max_portfolio_notional = snap.equity * limits_.max_position_pct_of_equity;
        if (projected_total_exposure > max_portfolio_notional)
            return deny(risk_rule::position_pct_of_equity, risk_action::reject);
    }

    // Phase 2.4 - spread circuit breaker (populated in Analytics from L2 snapshots when --depth-stream is active).
    // BF-01: reduce-only/exit orders must still pass here, exactly like the drawdown check above —
    // a blown-out spread is precisely when an open position most needs to be closable, not trapped.
    if (!reducing_exposure &&
        limits_.max_spread_bps > 0.0 && snap.current_spread_bps > limits_.max_spread_bps) {
        // Severe breaches (e.g. > 2x limit) escalate to halt to stop trading in obviously broken books
        if (snap.current_spread_bps > limits_.max_spread_bps * 2.0) {
            return deny(risk_rule::spread_limit, risk_action::halt);
        }
        return deny(risk_rule::spread_limit, risk_action::reject);
    }

    // Funding-rate circuit breaker. R3: the rate is only enforced when a
    // producer actually supplied one (derived from funding settlements) —
    // an unknown rate must not read as "0.0, therefore inside the limit".
    // BF-01: same reduce-only exemption as the spread breaker above.
    if (!reducing_exposure &&
        limits_.max_funding_8h_rate > 0.0 && snap.funding_rate_known &&
        snap.current_funding_8h_rate > limits_.max_funding_8h_rate) {
        if (snap.current_funding_8h_rate > limits_.max_funding_8h_rate * 1.5) {
            return deny(risk_rule::funding_limit, risk_action::halt);
        }
        return deny(risk_rule::funding_limit, risk_action::reject);
    }

    if (limits_.max_orders_per_minute > 0)
    {
        auto cutoff = order.get_timestamp() - std::chrono::seconds(60);
        prune_old_entries(order_timestamps_, cutoff);
        if (static_cast<int>(order_timestamps_.size()) >= limits_.max_orders_per_minute)
            return deny(risk_rule::orders_per_minute, risk_action::reject);
        order_timestamps_.push_back({order.get_timestamp()});
    }

    if (!reducing_exposure && limits_.max_daily_loss > 0.0)
    {
        update_daily_reset(order.get_timestamp());
        if (daily_loss_ >= limits_.max_daily_loss)
            return deny(risk_rule::daily_loss, risk_action::halt);
    }

    return risk_action::pass;
}

risk_action RiskManager::check_post_fill(const fill_event& fill,
                                         const portfolio& /* port */,
                                         const risk_snapshot& snap,
                                         risk_rule* rule_out)
{
    const auto deny = [&](risk_rule rule, risk_action action) {
        if (rule_out) *rule_out = rule;
        return action;
    };
    if (rule_out) *rule_out = risk_rule::none;

    if (snap.max_drawdown / 100.0 >= limits_.max_drawdown)
        return deny(risk_rule::drawdown, risk_action::halt);

    if (snap.has_last_trade &&
        snap.last_trade_pnl < -limits_.max_loss_per_trade)
        return deny(risk_rule::loss_per_trade, risk_action::halt);

    if (limits_.max_trades_per_hour > 0 &&
        static_cast<int>(trade_timestamps_.size()) >= limits_.max_trades_per_hour)
        return deny(risk_rule::trades_per_hour, risk_action::halt);

    if (limits_.max_daily_loss > 0.0)
    {
        update_daily_reset(fill.get_timestamp());
        // Include realized trading losses. Analytics trade PnL is already net
        // of opening/closing commissions, so do not add fill commissions again.
        if (snap.has_last_trade && snap.last_trade_seq != 0 &&
            snap.last_trade_pnl < 0.0 &&
            snap.last_trade_seq != last_daily_trade_seq_added_)
        {
            daily_loss_ += -snap.last_trade_pnl;
            last_daily_trade_seq_added_ = snap.last_trade_seq;
        }
        if (daily_loss_ >= limits_.max_daily_loss)
            return deny(risk_rule::daily_loss, risk_action::halt);
    }

    return risk_action::pass;
}

// Legacy overloads - collapse the heavy AnalyticsReport to the thin
// risk_snapshot and dispatch into the real path so logic lives in one
// place. R3: AnalyticsReport::total_orders/total_fills are reporting
// counters and are deliberately NOT copied into the risk snapshot.
risk_action RiskManager::check_order(const order_event& order,
                                     const portfolio& port,
                                     const AnalyticsReport& snap,
                                     std::size_t open_order_count,
                                     risk_rule* rule_out)
{
    risk_snapshot rs;
    rs.max_drawdown = snap.max_drawdown;
    // Phase 2 fields (best effort from full report; modern path uses risk_snapshot directly)
    rs.equity = snap.final_equity;  // approximate
    return check_order(order, port, rs, open_order_count, rule_out);
}

risk_action RiskManager::check_post_fill(const fill_event& fill,
                                         const portfolio& port,
                                         const AnalyticsReport& snap,
                                         risk_rule* rule_out)
{
    risk_snapshot rs;
    rs.max_drawdown = snap.max_drawdown;
    if (!snap.trades.empty())
	    {
	        rs.has_last_trade = true;
	        rs.last_trade_pnl = snap.trades.back().pnl;
	        rs.last_trade_seq = snap.trades.size();
	    }
    rs.equity = snap.final_equity;
    return check_post_fill(fill, port, rs, rule_out);
}

void RiskManager::on_fill(const fill_event& fill)
{
    if (limits_.max_trades_per_hour > 0)
    {
        auto cutoff = fill.get_timestamp() - std::chrono::hours(1);
        prune_old_entries(trade_timestamps_, cutoff);
        trade_timestamps_.push_back({fill.get_timestamp()});
    }

    if (limits_.max_daily_loss > 0.0)
        update_daily_reset(fill.get_timestamp());
}

// Phase A (MC object reuse)
void RiskManager::reset()
{
    order_timestamps_.clear();
    trade_timestamps_.clear();
    daily_loss_ = 0.0;
    daily_start_equity_ = 0.0;
    daily_reset_tp_ = {};
    last_daily_trade_seq_added_ = 0;
}
