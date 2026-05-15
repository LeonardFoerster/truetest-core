#pragma once

#include "../core/event.h"
#include "../execution/portfolio.h"

#include <cmath>
#include <cstdio>
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
//  - Cash proxy. The check uses portfolio.get_cash() as the available
//    margin. For isolated positions on a single symbol that's accurate;
//    for cross-margin or multi-symbol setups it's a coarse proxy.
//    Conservative when local cash < venue available_balance; optimistic
//    when it's higher. Operators with cross-margin risk should set
//    caps tighter than the strictly-necessary venue limits.
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

        // Maximum implied leverage = post_notional / cash. 0 disables.
        double max_leverage = 0.0;

        // Minimum projected post-trade liquidation buffer, as a
        // fraction of mark (e.g. 0.05 = 5%). 0 disables.
        double min_liquidation_distance_pct = 0.0;

        // Maintenance margin rate as a fraction (Binance BTCUSDT
        // first tier = 0.005). Used in the liquidation projection.
        double maintenance_margin_pct = 0.005;
    };

    explicit FuturesRiskCheck(const config& c) : c_(c) {}

    decision evaluate(const order_event& order,
                      const portfolio& port,
                      double mark_price) const override
    {
        decision out;
        if (mark_price <= 0.0) return out;
        if (c_.max_notional_usdt <= 0.0
            && c_.max_leverage <= 0.0
            && c_.min_liquidation_distance_pct <= 0.0)
            return out;

        const auto& positions = port.get_positions();
        const auto it = positions.find(order.get_symbol());
        const double existing_qty =
            (it != positions.end()) ? it->second.qty : 0.0;

        // Signed delta: BUY adds, SELL subtracts. Matches Binance
        // futures positionAmt sign convention (long > 0, short < 0).
        const double signed_delta = (order.get_side() == order_side::buy)
            ? order.get_quantity()
            : -order.get_quantity();

        const double post_qty      = existing_qty + signed_delta;
        const double post_notional = std::abs(post_qty) * mark_price;

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

        const double cash = port.get_cash();

        // Skip leverage / liquidation checks when cash <= 0 (no margin
        // base to compute against) or post_notional == 0 (closing to
        // flat is always safe for a leverage check).
        if (cash > 0.0 && post_notional > 0.0)
        {
            if (c_.max_leverage > 0.0)
            {
                const double implied_leverage = post_notional / cash;
                if (implied_leverage > c_.max_leverage)
                {
                    char buf[192];
                    std::snprintf(buf, sizeof(buf),
                        "FuturesRiskCheck: implied leverage %.2fx "
                        "exceeds cap %.2fx (notional=%.2f cash=%.2f)",
                        implied_leverage, c_.max_leverage,
                        post_notional, cash);
                    out.allow = false;
                    out.reason = buf;
                    return out;
                }
            }

            if (c_.min_liquidation_distance_pct > 0.0)
            {
                // Approx: distance ≈ cash / notional − maintenance_margin.
                // Equivalent to 1/L − mm. Used as the post-trade buffer
                // measured in units of mark price.
                const double margin_ratio = cash / post_notional;
                const double distance =
                    margin_ratio - c_.maintenance_margin_pct;
                if (distance < c_.min_liquidation_distance_pct)
                {
                    char buf[224];
                    std::snprintf(buf, sizeof(buf),
                        "FuturesRiskCheck: post-trade liquidation "
                        "distance %.2f%% < minimum %.2f%% "
                        "(cash=%.2f notional=%.2f mm=%.2f%%)",
                        distance * 100.0,
                        c_.min_liquidation_distance_pct * 100.0,
                        cash, post_notional,
                        c_.maintenance_margin_pct * 100.0);
                    out.allow = false;
                    out.reason = buf;
                    return out;
                }
            }
        }

        return out;
    }

    const config& cfg() const { return c_; }

private:
    config c_;
};
