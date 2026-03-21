#pragma once

#include "../core/event.h"
#include "../execution/portfolio.h"
#include "../analytics/analytics.h"

#include <cstdint>

struct risk_limits
{
    double max_position_value = 1e9;     // max notional per symbol
    double max_drawdown = 0.30;          // 30% max drawdown → halt
    double max_loss_per_trade = 10000.0; // single trade loss limit
    int max_open_orders = 1000;          // prevent order flooding
    double max_portfolio_exposure = 5e9; // total across all symbols
};

enum class risk_action { pass, reject, halt };

class RiskManager
{
public:
    explicit RiskManager(risk_limits limits = {});

    // Called before an order is submitted to the orderbook.
    risk_action check_order(const order_event& order,
                            const portfolio& port,
                            const AnalyticsReport& snap) const;

    // Called after a fill to check portfolio-level limits.
    risk_action check_post_fill(const fill_event& fill,
                                const portfolio& port,
                                const AnalyticsReport& snap) const;

private:
    risk_limits limits_;
};
