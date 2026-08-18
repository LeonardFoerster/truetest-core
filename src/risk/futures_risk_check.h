#pragma once

// ============================================================
// LIVE-SAFETY SURFACE — Phase 1 freeze (see prod.md)
// Any edit requires explicit two-person CCB review + 4 h
// mainnet shadow run on engine_shadow before merge.
// Authoritative path list: scripts/check-live-safety-freeze.sh
// ============================================================

#include "../core/event.h"
#include "../execution/portfolio.h"
#include "maintenance_margin_table.h"

using truetest::risk::MaintenanceMarginTable;

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

// Pre-trade risk check applied per outgoing order, in addition to the
// venue-agnostic RiskManager. Providers expose an instance via
// IProvider::get_risk_check(); the engine consults it before the
// existing RiskManager::check_order pass.
// Concrete subclasses are venue-specific. FuturesRiskCheck is the
// USDT-M futures impl. Spot has no separate check today (its rules
// are cash-bound and already covered by RiskManager + the portfolio's
// can_afford gate).
class IRiskCheck
{
public:
    struct decision
    {
        bool allow = true;
        std::string reason;
    };

    virtual ~IRiskCheck() = default;

    // mark_price: provider's last_mid_price for the symbol. Pass 0 when
    // unavailable; impls should treat that as "skip" (allow with empty
    // reason) rather than refusing — refusing on missing market data
    // would block the engine on every cold start before the first tick.
    virtual decision evaluate(const order_event& order,
                              const portfolio& port,
                              double mark_price) const = 0;

    // Engine-owned account context for cross-symbol portfolios. Generic
    // checks keep their existing contract; equity-aware checks override this
    // seam so the engine can supply per-symbol marked account equity without
    // moving venue policy into the composition root.
    virtual decision evaluate_with_account_equity(
        const order_event& order,
        const portfolio& port,
        double mark_price,
        double /*account_equity*/) const
    {
        return evaluate(order, port, mark_price);
    }
};

class NoopRiskCheck : public IRiskCheck
{
public:
    decision evaluate(const order_event&,
                      const portfolio&,
                      double) const override
    {
        return {};
    }
};

// USDT-M futures pre-trade risk: notional cap, leverage cap, projected
// post-trade liquidation distance.
// Important caveats encoded here (read before tuning the caps):
//  - Account equity. The engine supplies per-symbol marked equity through
//    evaluate_with_account_equity(). The three-argument evaluate() remains a
//    single-symbol compatibility path and derives equity from the given mark.
//  - Maintenance margin tiers. Binance USDT-M uses notional-tiered MM
//    rates (BTCUSDT: 0.5% up to 1M, then 0.65%, then …). This impl
//    uses a flat default (0.5%); raise via config for higher-notional
//    accounts where tiers actually bite.
//  - Pre-trade entry approximation. We don't know the entry price of
//    the new position, so the liquidation projection assumes entry ≈
//    mark. This is a small error for ack-fast venues like Binance
//    futures (mark moves under microseconds, fill happens within
//    seconds) but it's still an approximation; don't tune the
//    distance threshold below maintenance_margin + a safety factor.
class FuturesRiskCheck : public IRiskCheck
{
public:
    struct config
    {
        // Maximum |post_qty| × mark_price in USDT terms. 0 disables.
        double max_notional_usdt = 0.0;

        // Maximum implied leverage = post_notional / account equity. 0 disables.
        double max_leverage = 0.0;

        // Minimum projected post-trade liquidation buffer, as a
        // fraction of mark (e.g. 0.05 = 5%). 0 disables.
        double min_liquidation_distance_pct = 0.0;

        // Maintenance margin rate as a fraction (Binance BTCUSDT
        // first tier = 0.005). Used in the liquidation projection.
        // Overridden by the tiered table when provided.
        double maintenance_margin_pct = 0.005;
    };

    explicit FuturesRiskCheck(const config& c,
                              std::shared_ptr<MaintenanceMarginTable> mm_table = nullptr)
        : c_(c), mm_table_(std::move(mm_table)) {}

    decision evaluate(const order_event& order,
                      const portfolio& port,
                      double mark_price) const override
    {
        return evaluate_with_account_equity(
            order, port, mark_price, port.get_equity(mark_price));
    }

    decision evaluate_with_account_equity(
        const order_event& order,
        const portfolio& port,
        double mark_price,
        double account_equity) const override
    {
        decision out;
        if (c_.max_notional_usdt <= 0.0
            && c_.max_leverage <= 0.0
            && c_.min_liquidation_distance_pct <= 0.0)
            return out;

        const double order_qty = order.get_quantity();
        if (!std::isfinite(order_qty) || order_qty <= 0.0)
        {
            out.allow = false;
            out.reason = "FuturesRiskCheck: invalid non-positive order quantity";
            return out;
        }

        const auto& positions = port.get_positions();
        const auto it = positions.find(order.get_symbol());
        const double existing_qty =
            (it != positions.end()) ? it->second.qty : 0.0;
        if (!std::isfinite(existing_qty))
        {
            out.allow = false;
            out.reason = "FuturesRiskCheck: non-finite existing position";
            return out;
        }

        // Signed delta: BUY adds, SELL subtracts. Matches Binance
        // futures positionAmt sign convention (long > 0, short < 0).
        const double signed_delta = (order.get_side() == order_side::buy)
            ? order_qty
            : -order_qty;

        const double post_qty      = existing_qty + signed_delta;
        if (!std::isfinite(post_qty))
        {
            out.allow = false;
            out.reason = "FuturesRiskCheck: non-finite post-trade position";
            return out;
        }

        const double existing_abs = std::abs(existing_qty);
        const double post_abs = std::abs(post_qty);
        constexpr double qty_epsilon = 1e-12;
        const bool flattening = existing_abs > qty_epsilon
            && post_abs <= qty_epsilon;
        const bool same_direction_reduction = existing_abs > qty_epsilon
            && existing_qty * post_qty > 0.0
            && post_abs < existing_abs;

        // Reduction and flattening must never be blocked by an already-bad
        // account state or cap. Crossing through flat is deliberately not a
        // reduction: it closes one exposure and opens new opposite-side risk.
        if (flattening || same_direction_reduction)
            return out;

        if (!std::isfinite(c_.max_notional_usdt)
            || !std::isfinite(c_.max_leverage)
            || !std::isfinite(c_.min_liquidation_distance_pct)
            || !std::isfinite(c_.maintenance_margin_pct)
            || c_.max_notional_usdt < 0.0
            || c_.max_leverage < 0.0
            || c_.min_liquidation_distance_pct < 0.0
            || c_.min_liquidation_distance_pct > 1.0
            || c_.maintenance_margin_pct < 0.0)
        {
            out.allow = false;
            out.reason = "FuturesRiskCheck: invalid non-finite or negative "
                         "risk configuration";
            return out;
        }

        // A missing, non-finite, or non-positive mark makes every configured
        // notional/leverage/liquidation cap unknowable. Refuse new exposure
        // rather than converting bad market data into a cap bypass. Genuine
        // reductions returned above so operators can still de-risk.
        if (!std::isfinite(mark_price) || mark_price <= 0.0)
        {
            out.allow = false;
            out.reason = "FuturesRiskCheck: risk-increasing order refused "
                         "with invalid mark price";
            return out;
        }

        const double post_notional = std::abs(post_qty) * mark_price;
        if (!std::isfinite(post_notional))
        {
            out.allow = false;
            out.reason = "FuturesRiskCheck: non-finite post-trade notional";
            return out;
        }

        const bool crosses_flat = existing_qty * post_qty < 0.0;
        const bool increases_exposure = post_abs > existing_abs
            || crosses_flat;
        if (increases_exposure
            && (!std::isfinite(account_equity) || account_equity <= 0.0))
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "FuturesRiskCheck: risk-increasing order refused with "
                "non-positive equity %.2f (existing_qty=%.8f "
                "post_qty=%.8f mark=%.2f)",
                account_equity, existing_qty, post_qty, mark_price);
            out.allow = false;
            out.reason = buf;
            return out;
        }

        if (c_.max_notional_usdt > 0.0 &&
            post_notional > c_.max_notional_usdt)
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "FuturesRiskCheck: post-trade notional %.2f exceeds "
                "cap %.2f (existing_qty=%.8f delta=%.8f mark=%.2f)",
                post_notional, c_.max_notional_usdt,
                existing_qty, signed_delta, mark_price);
            out.allow = false;
            out.reason = buf;
            return out;
        }

        // A flat result has no leverage or liquidation distance. Reductions
        // returned above; risk-increasing orders with unusable equity were
        // refused fail-closed before reaching the divisions below.
        if (account_equity > 0.0 && post_notional > 0.0)
        {
            if (c_.max_leverage > 0.0)
            {
                const double implied_leverage = post_notional / account_equity;
                if (implied_leverage > c_.max_leverage)
                {
                    char buf[192];
                    std::snprintf(buf, sizeof(buf),
                        "FuturesRiskCheck: implied leverage %.2fx "
                        "exceeds cap %.2fx (notional=%.2f equity=%.2f)",
                        implied_leverage, c_.max_leverage,
                        post_notional, account_equity);
                    out.allow = false;
                    out.reason = buf;
                    return out;
                }
            }

            if (c_.min_liquidation_distance_pct > 0.0)
            {
                // Approx: distance ≈ equity / notional − maintenance_margin.
                // Equivalent to 1/L − mm. Used as the post-trade buffer
                // measured in units of mark price.
                double mm_rate = c_.maintenance_margin_pct;
                if (mm_table_ && !mm_table_->empty()) {
                    mm_rate = mm_table_->maintenance_margin_rate_for_notional(post_notional);
                }

                const double margin_ratio = account_equity / post_notional;
                const double distance = margin_ratio - mm_rate;

                if (distance < c_.min_liquidation_distance_pct)
                {
                    char buf[224];
                    std::snprintf(buf, sizeof(buf),
                        "FuturesRiskCheck: post-trade liquidation "
                        "distance %.2f%% < minimum %.2f%% "
                        "(equity=%.2f notional=%.2f mm=%.2f%%)",
                        distance * 100.0,
                        c_.min_liquidation_distance_pct * 100.0,
                        account_equity, post_notional,
                        mm_rate * 100.0);
                    out.allow = false;
                    out.reason = buf;
                    return out;
                }
            }
        }

        return out;
    }

    const config& cfg() const { return c_; }

    void set_maintenance_margin_table(std::shared_ptr<MaintenanceMarginTable> table) {
        mm_table_ = std::move(table);
    }

private:
    config c_;
    std::shared_ptr<MaintenanceMarginTable> mm_table_;
};
