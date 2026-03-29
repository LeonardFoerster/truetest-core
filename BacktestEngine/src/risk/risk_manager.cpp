#include "risk_manager.h"

#include <cmath>

RiskManager::RiskManager(risk_limits limits)
    : limits_(std::move(limits)) {}

risk_action RiskManager::check_order(const order_event& order,
                                     const portfolio& port,
                                     const AnalyticsReport& snap) const
{
    // 1. Max drawdown check → halt
    if (snap.max_drawdown / 100.0 >= limits_.max_drawdown)
        return risk_action::halt;

    // 2. Max open orders check → reject
    if (static_cast<int>(snap.total_orders - snap.total_fills) >= limits_.max_open_orders)
        return risk_action::reject;

    // 3. Max position value check (per-symbol notional) → reject
    if (order.get_side() == order_side::buy)
    {
        const auto& positions = port.get_positions();
        auto it = positions.find(order.get_symbol());
        double current_notional = 0.0;
        if (it != positions.end())
            current_notional = std::abs(it->second.cost_basis);

        double order_notional = order.get_quantity() * order.get_price();
        if (current_notional + order_notional > limits_.max_position_value)
            return risk_action::reject;
    }

    // 4. Max portfolio exposure check → reject
    if (order.get_side() == order_side::buy)
    {
        double total_exposure = 0.0;
        for (const auto& [sym, pos] : port.get_positions())
            total_exposure += std::abs(pos.cost_basis);

        double order_notional = order.get_quantity() * order.get_price();
        if (total_exposure + order_notional > limits_.max_portfolio_exposure)
            return risk_action::reject;
    }

    return risk_action::pass;
}

risk_action RiskManager::check_post_fill(const fill_event& /* fill */,
                                         const portfolio& /* port */,
                                         const AnalyticsReport& snap) const
{
    // 1. Max drawdown check → halt
    if (snap.max_drawdown / 100.0 >= limits_.max_drawdown)
        return risk_action::halt;

    // 2. Single trade loss limit → halt
    // Check the most recent trade's PnL from the analytics report
    if (!snap.trades.empty())
    {
        const auto& last_trade = snap.trades.back();
        if (last_trade.pnl < -limits_.max_loss_per_trade)
            return risk_action::halt;
    }

    return risk_action::pass;
}
