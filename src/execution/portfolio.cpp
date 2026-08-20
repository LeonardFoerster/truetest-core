#include "portfolio.h"
#include "../core/event.h"

#include <algorithm>
#include <cmath>

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
    double fill_qty = fill.get_filled_quantity();
    const double total_qty = fill_qty;
    const double price = fill.get_fill_price();
    // Prorate commission between the closing and opening legs of a flip so
    // it is charged exactly once per fill (pure closes/opens get the full fee).
    auto comm_for = [&](double q) {
        return total_qty > 1e-12 ? fill.get_commission() * (q / total_qty) : 0.0;
    };

    if (fill.get_side() == order_side::buy)
    {
        if (pos.qty < 0.0)
        {
            double close_qty = std::min(fill_qty, -pos.qty);
            // Release the closed fraction of the basis proportionally; this
            // preserves the per-unit entry of the remainder and is sign-safe
            // for shorts (whose cost_basis is negative).
            double frac_closed = close_qty / -pos.qty;

            cash_ -= close_qty * price + comm_for(close_qty);
            pos.qty += close_qty;
            pos.cost_basis *= (1.0 - frac_closed);

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

        pos.qty += fill_qty;
        pos.cost_basis += fill_qty * price + comm_for(fill_qty);
        cash_ -= fill_qty * price + comm_for(fill_qty);
    }
    else
    {
        if (pos.qty > 0.0)
        {
            double close_qty = std::min(fill_qty, pos.qty);
            double frac_closed = close_qty / pos.qty;

            cash_ += close_qty * price - comm_for(close_qty);
            pos.qty -= close_qty;
            pos.cost_basis *= (1.0 - frac_closed);

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

        pos.qty -= fill_qty;
        pos.cost_basis -= fill_qty * price - comm_for(fill_qty);
        cash_ += fill_qty * price - comm_for(fill_qty);
    }
}

void portfolio::apply_lot_fill(const fill_event& fill, std::uint64_t opener_order_id,
                               const std::string& strategy_name)
{
    constexpr double eps = 1e-12;
    const double fill_qty = fill.get_filled_quantity();
    if (fill_qty <= eps)
        return;

    // A physical fill can finish an old lot and open a residual lot in the
    // opposite direction. Keep the residual under the fill order itself so
    // later partial fills of that order continue the new lot.
    const auto add_to_fill_opener = [&](double open_qty)
    {
        if (open_qty <= eps)
            return;

        auto opener = lots_.find(fill.get_order_id());
        if (opener == lots_.end())
        {
            lot l;
            l.symbol           = fill.get_symbol();
            l.side             = fill.get_side();
            l.qty_open         = open_qty;
            l.entry_price      = fill.get_fill_price();
            l.entry_filled_qty = open_qty;
            l.strategy_name    = strategy_name;
            l.ts_open          = fill.get_timestamp();
            lots_.emplace(fill.get_order_id(), std::move(l));
        }
        else
        {
            auto& l = opener->second;
            const double new_filled = l.entry_filled_qty + open_qty;
            if (new_filled > 0.0)
                l.entry_price =
                    (l.entry_price * l.entry_filled_qty +
                     fill.get_fill_price() * open_qty) / new_filled;
            l.entry_filled_qty = new_filled;
            l.qty_open        += open_qty;
        }
    };

    if (opener_order_id == fill.get_order_id())
    {
        add_to_fill_opener(fill_qty);
        return;
    }

    auto old_opener = lots_.find(opener_order_id);
    if (old_opener == lots_.end())
    {
        // A later partial fill can arrive after an earlier fill consumed the
        // old opener exactly. No quantity remains to close, so this entire
        // physical fill is new exposure under its own order id. The same rule
        // keeps a stale attribution visible as an offsetting lot instead of
        // silently dropping a real fill from lot accounting.
        add_to_fill_opener(fill_qty);
        return;
    }

    // Clamp the close leg to the referenced lot. Any overshoot is real
    // opposite-side exposure and therefore becomes a new lot, rather than
    // driving qty_open negative and silently discarding the residual.
    const double close_qty = std::min(fill_qty,
                                      std::max(0.0, old_opener->second.qty_open));
    old_opener->second.qty_open -= close_qty;
    if (old_opener->second.qty_open <= eps)
        lots_.erase(old_opener);

    add_to_fill_opener(fill_qty - close_qty);
}

std::vector<std::uint64_t>
portfolio::open_lots_by_symbol(const std::string& symbol) const
{
    std::vector<std::uint64_t> out;
    for (const auto& [id, l] : lots_)
        if (l.symbol == symbol) out.push_back(id);
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

double portfolio::get_equity(const std::unordered_map<std::string, double>& marks,
                             double fallback_price) const
{
    double equity = cash_;
    for (const auto& [sym, pos] : positions_)
    {
        if (std::abs(pos.qty) <= 1e-12)
            continue;
        double px = fallback_price;
        if (auto it = marks.find(sym); it != marks.end() && it->second > 0.0)
            px = it->second;
        equity += pos.qty * px;
    }
    return equity;
}

double portfolio::get_equity(const std::unordered_map<std::string, mark_point>& marks,
                             double fallback_price) const
{
    double equity = cash_;
    for (const auto& [sym, pos] : positions_)
    {
        if (std::abs(pos.qty) <= 1e-12)
            continue;
        double px = fallback_price;
        if (auto it = marks.find(sym); it != marks.end() && it->second.usable())
            px = it->second.price;
        equity += pos.qty * px;
    }
    return equity;
}

double portfolio::get_strategy_position_qty(const std::string& strategy_name,
                                            const std::string& symbol) const
{
    constexpr double eps = 1e-12;
    double net = 0.0;
    for (const auto& [id, l] : lots_)
    {
        if (l.symbol == symbol && (strategy_name.empty() || l.strategy_name == strategy_name))
        {
            if (l.side == order_side::buy)
                net += l.qty_open;
            else
                net -= l.qty_open;
        }
    }
    return std::abs(net) > eps ? net : 0.0;
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
