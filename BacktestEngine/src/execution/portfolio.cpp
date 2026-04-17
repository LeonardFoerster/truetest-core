#include "portfolio.h"
#include "../core/event.h"

#include <cmath>
#include <iostream>

portfolio::portfolio() : initial_balance_(10000.0), cash_(10000.0) {}

portfolio::portfolio(double initial_balance)
    : initial_balance_(initial_balance), cash_(initial_balance) {}

void portfolio::on_fill(const fill_event& fill)
{
    total_fills_++;
    auto& pos = positions_[fill.get_symbol()];
    double fill_qty = fill.get_filled_quantity();

    if (fill.get_side() == order_side::buy)
    {
        if (pos.qty < 0.0)
        {
            // Closing (or flipping) a short position
            double close_qty = std::min(fill_qty, -pos.qty);
            double avg_entry = pos.cost_basis / pos.qty; // negative qty → negative basis per unit
            double cost_of_closed = avg_entry * close_qty; // portion of basis being closed

            cash_ -= close_qty * fill.get_fill_price() + fill.get_commission();
            pos.qty += close_qty;
            pos.cost_basis -= cost_of_closed;

            if (std::abs(pos.qty) < 1e-12)
            {
                pos.qty = 0.0;
                pos.cost_basis = 0.0;
                total_trades_++;
            }

            fill_qty -= close_qty;
            if (fill_qty < 1e-12)
                return;
        }

        // Opening or adding to a long position
        pos.qty += fill_qty;
        pos.cost_basis += fill_qty * fill.get_fill_price() + fill.get_commission();
        cash_ -= fill_qty * fill.get_fill_price() + fill.get_commission();
    }
    else // sell
    {
        if (pos.qty > 0.0)
        {
            // Closing (or flipping) a long position
            double close_qty = std::min(fill_qty, pos.qty);
            double avg_entry = (pos.qty > 0.0) ? pos.cost_basis / pos.qty : 0.0;
            double cost_of_closed = avg_entry * close_qty;

            cash_ += close_qty * fill.get_fill_price() - fill.get_commission();
            pos.qty -= close_qty;
            pos.cost_basis -= cost_of_closed;

            if (pos.qty < 1e-12)
            {
                pos.qty = 0.0;
                pos.cost_basis = 0.0;
                total_trades_++;
            }

            fill_qty -= close_qty;
            if (fill_qty < 1e-12)
                return;
        }

        // Opening or adding to a short position
        // cost_basis tracks the entry value (negative qty, negative basis)
        pos.qty -= fill_qty;
        pos.cost_basis -= fill_qty * fill.get_fill_price() - fill.get_commission();
        cash_ += fill_qty * fill.get_fill_price() - fill.get_commission();
    }
}

bool portfolio::position_open() const
{
    for (const auto& [_, pos] : positions_)
        if (std::abs(pos.qty) > 1e-12) return true;
    return false;
}

bool portfolio::position_open(const std::string& symbol) const
{
    auto it = positions_.find(symbol);
    return it != positions_.end() && std::abs(it->second.qty) > 1e-12;
}

bool portfolio::can_afford(order_side side, double quantity, double price) const
{
    if (side == order_side::buy)
        return cash_ >= quantity * price;
    // For sells without symbol context, check if any position suffices
    for (const auto& [_, pos] : positions_)
        if (pos.qty >= quantity) return true;
    return false;
}

bool portfolio::can_afford(const std::string& symbol, order_side side,
                           double quantity, double price) const
{
    if (side == order_side::buy)
        return cash_ >= quantity * price;

    auto it = positions_.find(symbol);
    return it != positions_.end() && it->second.qty >= quantity;
}

double portfolio::compute_quantity(double price, double risk_fraction) const
{
    if (price <= 0.0) return 0.0;
    return cash_ * risk_fraction / price;
}

double portfolio::get_equity(double last_price) const
{
    double equity = cash_;
    for (const auto& [_, pos] : positions_)
    {
        // Works for both long (qty > 0) and short (qty < 0) positions
        if (std::abs(pos.qty) > 1e-12)
            equity += pos.qty * last_price;
    }
    return equity;
}

void portfolio::print_summary() const
{
    std::cout << "Starting Balance: " << initial_balance_ << std::endl;
    std::cout << "Ending Cash: " << cash_ << std::endl;
    for (const auto& [sym, pos] : positions_)
    {
        if (std::abs(pos.qty) > 1e-12)
            std::cout << "Position " << sym << ": " << pos.qty << " units" << std::endl;
    }
    std::cout << "Total Trades Executed: " << total_trades_ << std::endl;
}
