#pragma once

#include "../types/price.h"
#include "../types/object_pool.h"

#include <list>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <string>
#include <algorithm>

class ControlBlockPool;

class order;
class order_modify;

enum class ob_order_type
{
    good_till_cancel,
    fill_or_kill,
    immediate_or_cancel
};

enum class side
{
    buy,
    sell
};

using quantity = std::uint64_t;
using order_id = std::uint64_t;

using order_pointer = std::shared_ptr<order>;
using order_pointers = std::list<order_pointer>;

struct lvl_info
{
    Price price_;
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
    order(ob_order_type Order_type, order_id Order_id_, side Side_, Price Price_, quantity Quantity_);

    order_id get_order_id() const;
    side get_side() const;
    Price get_price() const;
    ob_order_type get_order_type() const;
    quantity get_initial_quantity() const;
    quantity get_remaining_quantity() const;
    quantity get_filled_quantity() const;
    bool is_filled() const;
    void fill(quantity quantity);

private:
    ob_order_type order_type_;
    order_id order_id_;
    side side_;
    Price price_;
    quantity initial_quantity_;
    quantity remaining_quantity;
};

class order_modify
{
public:
    order_modify(order_id order_id, side side, Price price, quantity quantity);

    order_id get_order_id() const;
    Price get_price() const;
    side get_side() const;
    quantity get_quantity() const;
    order_pointer to_order_pointer(ob_order_type type) const;

private:
    order_id orderId_;
    Price price_;
    side side_;
    quantity quantity_;
};

struct trade_info
{
    order_id orderId_;
    Price price_;
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

struct order_node
{
    order_pointer order;
    order_node* next = nullptr;
    order_node* prev = nullptr;
};

struct price_level
{
    Price price;
    quantity total_qty = 0;
    order_node* head = nullptr;
    order_node* tail = nullptr;

    bool empty() const { return head == nullptr; }

    void append(order_node* n)
    {
        n->prev = tail;
        n->next = nullptr;
        if (tail) tail->next = n;
        else      head = n;
        tail = n;
        total_qty += n->order->get_remaining_quantity();
    }

    void remove(order_node* n)
    {
        if (n->prev) n->prev->next = n->next;
        else         head = n->next;
        if (n->next) n->next->prev = n->prev;
        else         tail = n->prev;
        total_qty -= n->order->get_remaining_quantity();
        n->prev = n->next = nullptr;
    }
};

class orderbook
{
public:
    // Phase 4: pooled order bodies + control blocks (wired from engine prewarm).
    void configure_order_pool(ControlBlockPool* cb_pool,
                              std::size_t min_blocks,
                              bool forbid_runtime_grow);

    order_pointer create_order(ob_order_type type, order_id id, side s,
                               Price price, quantity qty);

    trades add_order(order_pointer order);
    void cancel_order(order_id order_id);
    trades match_order(order_modify order);
    bool modify_order(order_id id, Price new_price, quantity new_qty);
    std::size_t size() const;
    orderbook_lvl_infos get_order_infos() const;

    void apply_l2_snapshot(const std::pair<Price, quantity>* bids, std::size_t bid_count,
                           const std::pair<Price, quantity>* asks, std::size_t ask_count);

    void apply_l2_snapshot(const std::vector<std::pair<Price, quantity>>& bids,
                           const std::vector<std::pair<Price, quantity>>& asks)
    {
        apply_l2_snapshot(bids.data(), bids.size(), asks.data(), asks.size());
    }
    void apply_l2_update(side side, Price price, quantity new_qty);
    void clear();

    ~orderbook() { clear(); }

private:
    ObjectPool<order> order_pool_;
    bool order_pool_ready_ = false;

    void ensure_order_pool_ready();
    static constexpr std::size_t NODE_BLOCK_SIZE = 4096;
    struct node_block { order_node nodes[NODE_BLOCK_SIZE]; };
    std::vector<std::unique_ptr<node_block>> node_blocks_;
    order_node* free_nodes_ = nullptr;

    order_node* alloc_node();
    void free_node(order_node* n);

    std::vector<price_level> bid_levels_;
    std::vector<price_level> ask_levels_;

    std::unordered_map<order_id, order_node*> order_map_;

    price_level& find_or_insert_level(std::vector<price_level>& levels, Price price, side s);
    void remove_level_if_empty(std::vector<price_level>& levels, Price price);

    bool can_match(side side, Price price) const;
    trades match_orders();
};
