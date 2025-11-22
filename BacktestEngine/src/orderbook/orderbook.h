#pragma once

//#include "../orderbook/orderbook.h"

#include <iostream>
#include <vector>
#include <list>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <numeric>
#include <memory>
#include <stdexcept>
#include <cstdint>
#include <string>

// Orderbook-specific order type; renamed to avoid collision with core/event.h order_type.
enum class ob_order_type
{
    good_till_cancel,
    fill_or_kill
};

enum class side
{
    buy,
    sell
};

using price = std::int32_t;
using quantity = std::uint64_t;
using order_id = std::uint64_t;

struct lvl_info
{
    price price_;
    quantity quantity_;
};


using lvl_infos = std::vector <lvl_info>;


class orderbook_lvl_infos
{
public:
    orderbook_lvl_infos(const lvl_infos& bids, const lvl_infos& asks)
        : bids_{ bids }
        , asks_{ asks }
    {
    }

    const lvl_infos& get_bids() const { return bids_; }
    const lvl_infos& get_asks() const { return asks_; }

private:

    lvl_infos bids_;
    lvl_infos asks_;

};

class order
{
public:
    order(ob_order_type Order_type, order_id Order_id_, side Side_, price Price_, quantity Quantity_)
        : order_type_{ Order_type }
        , order_id_{ Order_id_ }
        , side_{ Side_ }
        , price_{ Price_ }
        , initial_quantity_{ Quantity_ }
        , remaining_quantity{ Quantity_ }
    {
    }

    order_id get_order_id() const { return order_id_; }
    side get_side() const { return side_; }
    price get_price() const { return price_; }
    ob_order_type get_order_type() const { return order_type_; }
    quantity get_inital_quantity() const { return initial_quantity_; }
    quantity get_reamaining_quantity() const { return remaining_quantity; }
    quantity get_filled_quantity() const { return get_inital_quantity() - get_reamaining_quantity(); }
    bool is_filled() const { return get_reamaining_quantity() == 0; }
    void fill(quantity quantity)
    {
        if (quantity > get_reamaining_quantity())
        {
            throw std::logic_error("Order (" + std::to_string(get_order_id()) +
                ") cannot be filled for more than its remaning quantity");
        }

        remaining_quantity -= quantity;
    }

private:

    ob_order_type order_type_;
    order_id order_id_;
    side side_;
    price price_;
    quantity initial_quantity_;
    quantity remaining_quantity;

};


using order_pointer = std::shared_ptr<order>;
using order_pointers = std::list<order_pointer>;

class order_modify
{
public:
    order_modify(order_id order_id, side side, price price, quantity quantity)
        : orderId_{ order_id }
        , side_{ side }
        , price_{ price }
        , quantity_{ quantity }

    {
    }

    order_id get_order_id() const { return  orderId_; }
    price get_price() const { return price_; }
    side get_side() const { return side_; }
    quantity get_quantity() const { return quantity_; }

    order_pointer to_order_pointer(ob_order_type type) const
    {
        return std::make_shared<order>(type, get_order_id(), get_side(), get_price(), get_quantity());

    }
private:

    order_id orderId_;
    price price_;
    side side_;
    quantity quantity_;
};

struct trade_info
{
    order_id orderId_;
    price price_;
    quantity quantity_;
};

class trade
{
public:
    trade(const trade_info& bid_trade, const trade_info& ask_trade)
        : bid_trade_{ bid_trade }
        , ask_trade_{ ask_trade }
    {
    }

    const trade_info& get_bid_trade() const { return bid_trade_; }
    const trade_info& get_ask_trade() const { return ask_trade_; }

private:
    trade_info bid_trade_;
    trade_info ask_trade_;

};

using trades = std::vector<trade>;

class orderbook
{
private:
    struct order_entry
    {
        order_pointer order_{ nullptr };
        order_pointers::iterator location_;
    };

    std::map<price, order_pointers, std::greater<price>> bids_;
    std::map<price, order_pointers, std::less<price>> asks_;
    std::unordered_map<order_id, order_entry> orders_;

    bool can_match(side side, price price) const
    {
        if (side == side::buy)
        {
            if (asks_.empty())
            {
                return false;
            }

            const auto& [best_ask, _] = *asks_.begin();
            return price >= best_ask;
        }
        else
        {
            if (bids_.empty())
            {
                return false;
            }

            const auto& [best_bid, _] = *bids_.begin();
            return price <= best_bid;
        }
    }
    trades match_orders()
    {
        trades trades;
        trades.reserve(orders_.size());

        while (true)
        {
            if (bids_.empty() || asks_.empty())
            {
                break;
            }

            auto& [bid_price, bids] = *bids_.begin();
            auto& [ask_price, asks] = *asks_.begin();

            if (bid_price < ask_price)
            {
                break;
            }

            while (bids.size() && asks.size())
            {
                auto& bid = bids.front();
                auto& ask = asks.front();

                quantity quantity = std::min(bid->get_reamaining_quantity(), ask->get_reamaining_quantity());

                bid->fill(quantity);
                ask->fill(quantity);

                if (bid->is_filled())
                {
                    bids.pop_front();
                    orders_.erase(bid->get_order_id());
                }

                if (ask->is_filled())
                {
                    asks.pop_front();
                    orders_.erase(ask->get_order_id());
                }

                if (bids.empty())
                {
                    bids_.erase(bid_price);
                    break;
                }

                if (asks.empty())
                {
                    asks_.erase(ask_price);
                    break;
                }

                trade_info bid_trade{ bid->get_order_id(), bid->get_price(), quantity };
                trade_info ask_trade{ ask->get_order_id(), ask->get_price(), quantity };
                trades.push_back(trade{ bid_trade, ask_trade });

            }

        }

        if (!bids_.empty())
        {
            auto& [_, bids] = *bids_.begin();
            auto& order = bids.front();

            if (order->get_order_type() == ob_order_type::fill_or_kill)
            {
                cancel_order(order->get_order_id());
            }
        }
        if (!asks_.empty())
        {
            auto& [_, asks] = *asks_.begin();
            auto& order = asks.front();

            if (order->get_order_type() == ob_order_type::fill_or_kill)
            {
                cancel_order(order->get_order_id());
            }
        }

        return trades;
    }

public:
    trades add_order(order_pointer order)
    {
        if (orders_.find(order->get_order_id()) != orders_.end())
        {
            return { };
        }

        if (order->get_order_type() == ob_order_type::fill_or_kill && !can_match(order->get_side(), order->get_price()))
        {
            return { };
        }

        order_pointers::iterator iterator;

        if (order->get_side() == side::buy)
        {
            auto& orders = bids_[order->get_price()];
            orders.push_back(order);
            iterator = std::next(orders.begin(), orders.size() - 1);
        }
        else
        {
            auto& orders = asks_[order->get_price()];
            orders.push_back(order);
            iterator = std::next(orders.begin(), orders.size() - 1);
        }

        orders_.insert({ order->get_order_id(), order_entry{order, iterator} });
        return match_orders();
    }

    void cancel_order(order_id order_id)
    {
        if (orders_.find(order_id) == orders_.end())
        {
            return;
        }

        const auto& [order, order_iterator] = orders_.at(order_id);
        orders_.erase(order_id);

        if (order->get_side() == side::sell)
        {
            auto price = order->get_price();
            auto& orders = asks_.at(price);
            orders.erase(order_iterator);

            if (orders.empty())
            {
                asks_.erase(price);
            }
        }
        else
        {
            auto price = order->get_price();
            auto& orders = bids_.at(price);
            orders.erase(order_iterator);

            if (orders.empty())
            {
                bids_.erase(price);
            }

        }


    }

    trades match_order(order_modify order)
    {
        if (orders_.find(order.get_order_id()) == orders_.end())
        {
            return { };
        }

        const auto& [existing_order, _] = orders_.at(order.get_order_id());
        cancel_order(order.get_order_id());
        return add_order(order.to_order_pointer(existing_order->get_order_type()));
    }

    std::size_t size() const { return orders_.size(); }

    orderbook_lvl_infos get_order_infos() const
    {
        lvl_infos bid_infos, ask_infos;
        bid_infos.reserve(orders_.size());
        ask_infos.reserve(orders_.size());

        auto create_lvl_infos = [](price price, const order_pointers& orders)
            {
                return lvl_info{ price, std::accumulate(orders.begin(), orders.end(), (quantity)0,
                    [](quantity running_sum, const order_pointer& order)
                { return running_sum + order->get_reamaining_quantity(); })
                };

            };

        for (const auto& [price, orders] : bids_)
        {
            bid_infos.push_back(create_lvl_infos(price, orders));
        }

        for (const auto& [price, orders] : asks_)
        {
            ask_infos.push_back(create_lvl_infos(price, orders));
        }

        return orderbook_lvl_infos{ bid_infos, ask_infos };
    }
};
