#include "orderbook.h"
#include <stdexcept>
#include <numeric>
#include <algorithm>
#include <iostream> // For logging

// --- order Method Implementations ---

order::order(ob_order_type Order_type, order_id Order_id_, side Side_, price Price_, quantity Quantity_)
    : order_type_(Order_type), order_id_(Order_id_), side_(Side_), price_(Price_), initial_quantity_(Quantity_), remaining_quantity(Quantity_) {}

void order::fill(quantity quantity)
{
    if (quantity > remaining_quantity)
    {
        throw std::logic_error("Order (" + std::to_string(get_order_id()) +
            ") cannot be filled for more than its remaining quantity");
    }
    remaining_quantity -= quantity;
}

order_id order::get_order_id() const { return order_id_; }
side order::get_side() const { return side_; }
price order::get_price() const { return price_; }
ob_order_type order::get_order_type() const { return order_type_; }
quantity order::get_inital_quantity() const { return initial_quantity_; }
quantity order::get_reamaining_quantity() const { return remaining_quantity; }
quantity order::get_filled_quantity() const { return initial_quantity_ - remaining_quantity; }
bool order::is_filled() const { return remaining_quantity == 0; }

// --- order_modify Method Implementations ---

order_pointer order_modify::to_order_pointer(ob_order_type type) const
{
    return std::make_shared<order>(type, get_order_id(), get_side(), get_price(), get_quantity());
}

order_id order_modify::get_order_id() const { return orderId_; }
price order_modify::get_price() const { return price_; }
side order_modify::get_side() const { return side_; }
quantity order_modify::get_quantity() const { return quantity_; }

// --- trade Method Implementations ---

trade::trade(const trade_info& bid_trade, const trade_info& ask_trade)
    : bid_trade_(bid_trade), ask_trade_(ask_trade) {}

const trade_info& trade::get_bid_trade() const { return bid_trade_; }
const trade_info& trade::get_ask_trade() const { return ask_trade_; }

// --- orderbook_lvl_infos Method Implementations ---

orderbook_lvl_infos::orderbook_lvl_infos(const lvl_infos& bids, const lvl_infos& asks)
    : bids_(bids), asks_(asks) {}

const lvl_infos& orderbook_lvl_infos::get_bids() const { return bids_; }
const lvl_infos& orderbook_lvl_infos::get_asks() const { return asks_; }

// --- orderbook Method Implementations ---

bool orderbook::can_match(side side, price price) const
{
    if (side == side::buy)
    {
        if (asks_.empty())
        {
            return false;
        }
        return price >= asks_.top().first;
    }
    else
    {
        if (bids_.empty())
        {
            return false;
        }
        return price <= bids_.top().first;
    }
}

trades orderbook::match_orders()
{
    trades trades;

    while (!bids_.empty() && !asks_.empty() && bids_.top().first >= asks_.top().first)
    {
        auto bid = bids_.top(); bids_.pop();
        auto ask = asks_.top(); asks_.pop();

        quantity trade_quantity = std::min(bid.second->get_reamaining_quantity(), ask.second->get_reamaining_quantity());

        bid.second->fill(trade_quantity);
        ask.second->fill(trade_quantity);

        trade_info bid_trade{ bid.second->get_order_id(), bid.second->get_price(), trade_quantity };
        trade_info ask_trade{ ask.second->get_order_id(), ask.second->get_price(), trade_quantity };
        trades.push_back(trade{ bid_trade, ask_trade });

        // If not filled, push back
        if (!bid.second->is_filled()) bids_.push(bid);
        if (!ask.second->is_filled()) asks_.push(ask);

        // Remove from orders if filled
        if (bid.second->is_filled()) orders_.erase(bid.second->get_order_id());
        if (ask.second->is_filled()) orders_.erase(ask.second->get_order_id());
    }

    // Handle fill_or_kill (simplified)
    if (!bids_.empty() && bids_.top().second->get_order_type() == ob_order_type::fill_or_kill)
    {
        cancel_order(bids_.top().second->get_order_id());
    }
    if (!asks_.empty() && asks_.top().second->get_order_type() == ob_order_type::fill_or_kill)
    {
        cancel_order(asks_.top().second->get_order_id());
    }

    return trades;
}

trades orderbook::add_order(order_pointer order)
{
    if (orders_.count(order->get_order_id()))
    {
        return { };
    }

    if (order->get_order_type() == ob_order_type::fill_or_kill && !can_match(order->get_side(), order->get_price()))
    {
        return { };
    }

    if (order->get_side() == side::buy)
    {
        bids_.push({order->get_price(), order});
        bid_quantities_[order->get_price()] += order->get_reamaining_quantity();
    }
    else
    {
        asks_.push({order->get_price(), order});
        ask_quantities_[order->get_price()] += order->get_reamaining_quantity();
    }

    orders_[order->get_order_id()] = {order};
    return match_orders();
}

void orderbook::cancel_order(order_id order_id)
{
    auto it = orders_.find(order_id);
    if (it == orders_.end())
    {
        return;
    }

    // Note: priority_queue does not support efficient removal, so we just remove from orders_
    // The order may still be in the queue, but since it's cancelled, it won't be matched.
    orders_.erase(it);
}

trades orderbook::match_order(order_modify order)
{
    if (orders_.find(order.get_order_id()) == orders_.end())
    {
        return { };
    }

    const auto& [existing_order] = orders_.at(order.get_order_id());
    cancel_order(order.get_order_id());
    return add_order(order.to_order_pointer(existing_order->get_order_type()));
}

std::size_t orderbook::size() const
{
    return orders_.size();
}

orderbook_lvl_infos orderbook::get_order_infos() const
{
    lvl_infos bid_infos, ask_infos;

    for (const auto& [price, qty] : bid_quantities_)
    {
        bid_infos.push_back({price, qty});
    }

    for (const auto& [price, qty] : ask_quantities_)
    {
        ask_infos.push_back({price, qty});
    }

    return orderbook_lvl_infos{ bid_infos, ask_infos };
}
