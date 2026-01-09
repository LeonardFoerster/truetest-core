#pragma once

#include <iostream>
#include <vector>
#include <list>
#include <map>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <string>
#include <queue>

// Forward declarations
class order;
class order_modify;

// Orderbook-specific order type
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

using order_pointer = std::shared_ptr<order>;
using order_pointers = std::list<order_pointer>;

struct BidComparator {
    bool operator()(const std::pair<price, order_pointer>& a, const std::pair<price, order_pointer>& b) const {
        return a.first < b.first; // max heap
    }
};

struct AskComparator {
    bool operator()(const std::pair<price, order_pointer>& a, const std::pair<price, order_pointer>& b) const {
        return a.first > b.first; // min heap
    }
};

struct lvl_info
{
    price price_;
    quantity quantity_;
};

using lvl_infos = std::vector<lvl_info>;

class orderbook_lvl_infos
{
public:
    orderbook_lvl_infos(const lvl_infos& bids, const lvl_infos& asks);
    const lvl_infos& get_bids() const;
    const lvl_infos& get_asks() const;
private:
    lvl_infos bids_;
    lvl_infos asks_;
};

class order
{
public:
    order(ob_order_type Order_type, order_id Order_id_, side Side_, price Price_, quantity Quantity_);

    order_id get_order_id() const;
    side get_side() const;
    price get_price() const;
    ob_order_type get_order_type() const;
    quantity get_inital_quantity() const;
    quantity get_reamaining_quantity() const;
    quantity get_filled_quantity() const;
    bool is_filled() const;
    void fill(quantity quantity);

private:
    ob_order_type order_type_;
    order_id order_id_;
    side side_;
    price price_;
    quantity initial_quantity_;
    quantity remaining_quantity;
};

class order_modify
{
public:
    order_modify(order_id order_id, side side, price price, quantity quantity);

    order_id get_order_id() const;
    price get_price() const;
    side get_side() const;
    quantity get_quantity() const;
    order_pointer to_order_pointer(ob_order_type type) const;

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
    trade(const trade_info& bid_trade, const trade_info& ask_trade);
    const trade_info& get_bid_trade() const;
    const trade_info& get_ask_trade() const;

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
    };

    std::priority_queue<std::pair<price, order_pointer>, std::vector<std::pair<price, order_pointer>>, BidComparator> bids_;
    std::priority_queue<std::pair<price, order_pointer>, std::vector<std::pair<price, order_pointer>>, AskComparator> asks_;
    std::unordered_map<order_id, order_entry> orders_;
    std::map<price, quantity> bid_quantities_;
    std::map<price, quantity> ask_quantities_;

    bool can_match(side side, price price) const;
    trades match_orders();

public:
    trades add_order(order_pointer order);
    void cancel_order(order_id order_id);
    trades match_order(order_modify order);
    std::size_t size() const;
    orderbook_lvl_infos get_order_infos() const;
};

