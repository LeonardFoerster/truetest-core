#include "portfolio.h"
#include "../core/event.h"
#include "position_sizing.h"

#include <cmath>
#include <iostream>

portfolio::portfolio() : initial_balance_(10000.0), cash_(10000.0) {}

portfolio::portfolio(double initial_balance)
    : initial_balance_(initial_balance), cash_(initial_balance) {}

void portfolio::on_fill(const fill_event& fill)
{
    // Legacy path (pre-deepdive per-lot consolidation). Delegates to rich
    // version using the fill's own order_id as opener (correct for simple
    // single-lot openers; for closers and multi-lot the caller should use the
    // 3-arg overload with the true opener_order_id + strategy_name).
    on_fill(fill, fill.get_opener_order_id() != 0 ? fill.get_opener_order_id() : fill.get_order_id(),
            fill.get_strategy_name());
}

void portfolio::on_fill(const fill_event& fill,
                        std::uint64_t opener_order_id,
                        const std::string& strategy_name)
{
    total_fills_++;
    apply_netted_fill(fill);
    if (opener_order_id != 0)
        apply_lot_fill(fill, opener_order_id, strategy_name);
}

void portfolio::on_funding(const funding_event& fe)
{
    cash_ += fe.get_cash_delta();
    total_funding_pnl_ += fe.get_cash_delta();
    // Note: we do not touch lots_ here. Funding is a pure cash adjustment.
}

void portfolio::apply_netted_fill(const fill_event& fill)
{
    auto& pos = positions_[fill.get_symbol()];
    const double original_fill_qty = fill.get_filled_quantity();
    const double fill_commission = fill.get_commission();
    auto prorated_commission = [&](double qty) {
        return original_fill_qty > 0.0 ? fill_commission * qty / original_fill_qty : 0.0;
    };
    double fill_qty = original_fill_qty;

    if (fill.get_side() == order_side::buy)
    {
        if (pos.qty < 0.0)
        {
            double close_qty = std::min(fill_qty, -pos.qty);
            double avg_entry = pos.cost_basis / pos.qty;
            double cost_of_closed = avg_entry * close_qty;
            double close_commission = prorated_commission(close_qty);

            cash_ -= close_qty * fill.get_fill_price() + close_commission;
            pos.qty += close_qty;
            pos.cost_basis += cost_of_closed;

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

        double open_commission = prorated_commission(fill_qty);
        pos.qty += fill_qty;
        pos.cost_basis += fill_qty * fill.get_fill_price() + open_commission;
        cash_ -= fill_qty * fill.get_fill_price() + open_commission;
    }
    else
    {
        if (pos.qty > 0.0)
        {
            double close_qty = std::min(fill_qty, pos.qty);
            double avg_entry = (pos.qty > 0.0) ? pos.cost_basis / pos.qty : 0.0;
            double cost_of_closed = avg_entry * close_qty;
            double close_commission = prorated_commission(close_qty);

            cash_ += close_qty * fill.get_fill_price() - close_commission;
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

        double open_commission = prorated_commission(fill_qty);
        pos.qty -= fill_qty;
        // Include commission in cost_basis so abs(cost_basis) ≈ notional + comm
        // for both directions (was asymmetric for shorts, making avg_entry and
        // notional checks slightly inconsistent).
        pos.cost_basis -= fill_qty * fill.get_fill_price() + open_commission;
        cash_ += fill_qty * fill.get_fill_price() - open_commission;
    }
}

void portfolio::apply_lot_fill(const fill_event& fill, std::uint64_t opener_order_id,
                               const std::string& strategy_name)
{
    const bool is_opener = (opener_order_id == fill.get_order_id());
    auto it = lots_.find(opener_order_id);

    if (is_opener)
    {
        if (it == lots_.end())
        {
            lot l;
            l.symbol           = fill.get_symbol();
            l.side             = fill.get_side();
            l.qty_open         = fill.get_filled_quantity();
            l.entry_price      = fill.get_fill_price();
            l.entry_filled_qty = fill.get_filled_quantity();
            l.strategy_name    = strategy_name;
            l.ts_open          = fill.get_timestamp();
            lots_.emplace(opener_order_id, std::move(l));
        }
        else
        {
            // Partial fills on the same opener - roll into weighted avg.
            auto& l = it->second;
            double new_filled = l.entry_filled_qty + fill.get_filled_quantity();
            if (new_filled > 0.0)
                l.entry_price =
                    (l.entry_price * l.entry_filled_qty +
                     fill.get_fill_price() * fill.get_filled_quantity()) / new_filled;
            l.entry_filled_qty = new_filled;
            l.qty_open        += fill.get_filled_quantity();
        }
        return;
    }

    // Closer: reduce the referenced lot. A closer with no matching lot is a
    // stale reference - portfolio state is authoritative, so drop silently.
    if (it == lots_.end()) return;
    auto& l = it->second;
    l.qty_open -= fill.get_filled_quantity();
    if (l.qty_open < 1e-12)
        lots_.erase(it);
}

std::vector<std::uint64_t>
portfolio::open_lots_by_symbol(const std::string& symbol) const
{
    std::vector<std::uint64_t> out;
    for (const auto& [id, l] : lots_)
        if (l.symbol == symbol) out.push_back(id);
    return out;
}

std::vector<std::uint64_t>
portfolio::open_lots_by_strategy(const std::string& strategy_name) const
{
    std::vector<std::uint64_t> out;
    for (const auto& [id, l] : lots_)
        if (l.strategy_name == strategy_name) out.push_back(id);
    return out;
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

bool portfolio::can_afford(order_side side, double quantity, double price,
                           double commission) const
{
    if (side == order_side::buy)
        return cash_ >= quantity * price + std::max(0.0, commission);
    for (const auto& [_, pos] : positions_)
        if (pos.qty >= quantity) return true;
    return false;
}

bool portfolio::can_afford(const std::string& symbol, order_side side,
                           double quantity, double price, double commission) const
{
    if (side == order_side::buy)
        return cash_ >= quantity * price + std::max(0.0, commission);

    auto it = positions_.find(symbol);
    return it != positions_.end() && it->second.qty >= quantity;
}

double portfolio::compute_quantity(double price, double risk_fraction,
                                   double entry_fee_rate) const
{
    return truetest::risk::compute_notional_quantity(
        cash_, risk_fraction, price, entry_fee_rate);
}

double portfolio::get_equity(double last_price) const
{
    double equity = cash_;
    for (const auto& [_, pos] : positions_)
    {
        if (std::abs(pos.qty) > 1e-12)
            equity += pos.qty * last_price;
    }
    return equity;
}

double portfolio::get_equity(
    const std::unordered_map<std::string, double>& marks_by_symbol) const
{
    double equity = cash_;
    for (const auto& [sym, pos] : positions_)
    {
        if (std::abs(pos.qty) <= 1e-12)
            continue;
        auto it = marks_by_symbol.find(sym);
        if (it != marks_by_symbol.end())
            equity += pos.qty * it->second;
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
    std::cout << "Open Lots: " << lots_.size() << std::endl;
}

// Phase A (MC object reuse): reset to initial constructed state.
void portfolio::reset()
{
    cash_ = initial_balance_;
    positions_.clear();
    lots_.clear();
    total_trades_ = 0;
    total_fills_ = 0;
    total_funding_pnl_ = 0.0;
}
