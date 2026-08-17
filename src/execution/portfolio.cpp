#include "portfolio.h"
#include "../core/event.h"
#include "../core/fill_validation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kQtyEps = 1e-12;

struct netted_fill_preview
{
    position pos{};
    double cash = 0.0;
    bool closed_trade = false;
};

bool preview_netted_fill(const position* current, double current_cash,
                         const fill_event& fill,
                         netted_fill_preview& out) noexcept
{
    using namespace fill_validation;
    if (!fill_validation::finite(current_cash)) return false;

    out.pos = current ? *current : position{};
    out.cash = current_cash;
    if (!fill_validation::finite(out.pos.qty) || !fill_validation::finite(out.pos.cost_basis)) return false;

    const double total_qty = fill.get_filled_quantity();
    const double price = fill.get_fill_price();
    const double commission = fill.get_commission();
    double qty_left = total_qty;

    const auto commission_for = [&](double qty, double& value) noexcept {
        double fraction = 0.0;
        return checked_div(qty, total_qty, fraction) &&
               checked_mul(commission, fraction, value);
    };
    const auto close_position = [&](double close_qty, double previous_abs,
                                    bool buy) noexcept {
        double notional = 0.0;
        double comm = 0.0;
        double cash_delta = 0.0;
        double fraction = 0.0;
        double remainder = 0.0;
        if (!checked_notional(close_qty, price, notional) ||
            !commission_for(close_qty, comm) ||
            !checked_div(close_qty, previous_abs, fraction) ||
            !checked_sub(1.0, fraction, remainder))
            return false;

        if (buy)
        {
            if (!checked_add(notional, comm, cash_delta) ||
                !checked_sub(out.cash, cash_delta, out.cash) ||
                !checked_add(out.pos.qty, close_qty, out.pos.qty))
                return false;
        }
        else
        {
            if (!checked_sub(notional, comm, cash_delta) ||
                !checked_add(out.cash, cash_delta, out.cash) ||
                !checked_sub(out.pos.qty, close_qty, out.pos.qty))
                return false;
        }
        return checked_mul(out.pos.cost_basis, remainder, out.pos.cost_basis);
    };

    if (fill.get_side() == order_side::buy)
    {
        if (out.pos.qty < 0.0)
        {
            const double previous_abs = -out.pos.qty;
            const double close_qty = std::min(qty_left, previous_abs);
            if (!close_position(close_qty, previous_abs, true)) return false;
            if (std::abs(out.pos.qty) < kQtyEps)
            {
                out.pos.qty = 0.0;
                out.pos.cost_basis = 0.0;
                out.closed_trade = true;
            }
            if (!checked_sub(qty_left, close_qty, qty_left)) return false;
            if (qty_left < kQtyEps) return true;
        }

        double notional = 0.0;
        double comm = 0.0;
        double total = 0.0;
        if (!checked_notional(qty_left, price, notional) ||
            !commission_for(qty_left, comm) ||
            !checked_add(notional, comm, total) ||
            !checked_add(out.pos.qty, qty_left, out.pos.qty) ||
            !checked_add(out.pos.cost_basis, total, out.pos.cost_basis) ||
            !checked_sub(out.cash, total, out.cash))
            return false;
    }
    else
    {
        if (out.pos.qty > 0.0)
        {
            const double previous_abs = out.pos.qty;
            const double close_qty = std::min(qty_left, previous_abs);
            if (!close_position(close_qty, previous_abs, false)) return false;
            if (out.pos.qty < kQtyEps)
            {
                out.pos.qty = 0.0;
                out.pos.cost_basis = 0.0;
                out.closed_trade = true;
            }
            if (!checked_sub(qty_left, close_qty, qty_left)) return false;
            if (qty_left < kQtyEps) return true;
        }

        double notional = 0.0;
        double comm = 0.0;
        double total = 0.0;
        if (!checked_notional(qty_left, price, notional) ||
            !commission_for(qty_left, comm) ||
            !checked_sub(notional, comm, total) ||
            !checked_sub(out.pos.qty, qty_left, out.pos.qty) ||
            !checked_sub(out.pos.cost_basis, total, out.pos.cost_basis) ||
            !checked_add(out.cash, total, out.cash))
            return false;
    }
    return fill_validation::finite(out.pos.qty) && fill_validation::finite(out.pos.cost_basis) &&
           fill_validation::finite(out.cash);
}

bool preview_lot_fill(const std::unordered_map<std::uint64_t, lot>& lots,
                      const fill_event& fill, std::uint64_t opener_order_id) noexcept
{
    using namespace fill_validation;
    if (opener_order_id == 0) return true;
    const bool is_opener = opener_order_id == fill.get_order_id();
    const auto it = lots.find(opener_order_id);
    if (is_opener)
    {
        if (it == lots.end()) return true;
        const auto& l = it->second;
        if (!fill_validation::finite(l.qty_open) || !fill_validation::finite(l.entry_price) ||
            !fill_validation::finite(l.entry_filled_qty))
            return false;
        double new_filled = 0.0;
        double prior_value = 0.0;
        double fill_value = 0.0;
        double total_value = 0.0;
        double average = 0.0;
        double qty_open = 0.0;
        return checked_add(l.entry_filled_qty, fill.get_filled_quantity(), new_filled) &&
               checked_mul(l.entry_price, l.entry_filled_qty, prior_value) &&
               checked_mul(fill.get_fill_price(), fill.get_filled_quantity(), fill_value) &&
               checked_add(prior_value, fill_value, total_value) &&
               checked_div(total_value, new_filled, average) &&
               checked_add(l.qty_open, fill.get_filled_quantity(), qty_open);
    }
    if (it == lots.end()) return true;
    if (!fill_validation::finite(it->second.qty_open)) return false;
    double remaining = 0.0;
    return checked_sub(it->second.qty_open, fill.get_filled_quantity(), remaining);
}

} // namespace

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
    if (!can_apply_fill(fill, opener_order_id, strategy_name))
        return;
    total_fills_++;
    apply_netted_fill(fill);
    if (opener_order_id != 0)
        apply_lot_fill(fill, opener_order_id, strategy_name);
}

bool portfolio::can_apply_fill(const fill_event& fill,
                               std::uint64_t opener_order_id,
                               const std::string& /* strategy_name */) const noexcept
{
    using namespace fill_validation;
    if (!valid_fill_shape(fill) || !has_finite_state()) return false;
    std::size_t ignored = 0;
    if (!checked_size_increment(total_fills_, ignored)) return false;

    const auto position_it = positions_.find(fill.get_symbol());
    netted_fill_preview preview;
    if (!preview_netted_fill(position_it == positions_.end() ? nullptr : &position_it->second,
                             cash_, fill, preview))
        return false;
    if (preview.closed_trade && !checked_size_increment(total_trades_, ignored))
        return false;
    return preview_lot_fill(lots_, fill, opener_order_id);
}

bool portfolio::has_finite_state() const noexcept
{
    using namespace fill_validation;
    if (!fill_validation::finite(initial_balance_) || !fill_validation::finite(cash_) ||
        !fill_validation::finite(total_funding_pnl_))
        return false;
    for (const auto& [_, pos] : positions_)
        if (!fill_validation::finite(pos.qty) || !fill_validation::finite(pos.cost_basis)) return false;
    for (const auto& [_, l] : lots_)
        if (!fill_validation::finite(l.qty_open) || !fill_validation::finite(l.entry_price) ||
            !fill_validation::finite(l.entry_filled_qty))
            return false;
    return true;
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
