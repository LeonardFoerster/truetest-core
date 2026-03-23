#include "portfolio.h"
#include "../core/event.h"

#include <cmath>
#include <iostream>

portfolio::portfolio() : initial_balance_(10000.0), cash_(10000.0) {}

portfolio::portfolio(double initial_balance)
    : initial_balance_(initial_balance), cash_(initial_balance) {}

void portfolio::on_fill(const fill_event& fill)
{
    auto& pos = positions_[fill.get_symbol()];

    if (fill.get_side() == order_side::buy)
    {
        pos.qty += fill.get_filled_quantity();
        pos.cost_basis += fill.get_total_cost();
        cash_ -= fill.get_total_cost();
    }
    else if (fill.get_side() == order_side::sell && pos.qty > 0.0)
    {
        double sell_qty = std::min(fill.get_filled_quantity(), pos.qty);

        // Compute average entry price for the portion being sold
        double avg_entry = (pos.qty > 0.0) ? pos.cost_basis / pos.qty : 0.0;
        double cost_of_sold = avg_entry * sell_qty;

        cash_ += fill.get_total_cost();
        pos.qty -= sell_qty;
        pos.cost_basis -= cost_of_sold;

        if (pos.qty <= 0.0)
        {
            pos.qty = 0.0;
            pos.cost_basis = 0.0;
            total_trades_++;
        }
    }
}

bool portfolio::position_open() const
{
    for (const auto& [_, pos] : positions_)
        if (pos.qty > 0.0) return true;
    return false;
}

bool portfolio::position_open(const std::string& symbol) const
{
    auto it = positions_.find(symbol);
    return it != positions_.end() && it->second.qty > 0.0;
}

bool portfolio::can_afford(order_side side, double quantity, double price) const
{
    if (side == order_side::buy)
        return cash_ >= quantity * price;
    // For sells, check we have the position
    for (const auto& [_, pos] : positions_)
        if (pos.qty >= quantity) return true;
    return false;
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
        if (pos.qty > 0.0)
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
        if (pos.qty > 0.0)
            std::cout << "Position " << sym << ": " << pos.qty << " units" << std::endl;
    }
    std::cout << "Total Trades Executed: " << total_trades_ << std::endl;
}
