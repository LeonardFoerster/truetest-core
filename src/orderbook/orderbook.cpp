#include "orderbook.h"
#include "../types/control_block_pool.h"
#include "../types/order_id.h"
#include <stdexcept>
#include <algorithm>

order::order(ob_order_type Order_type, order_id Order_id_, side Side_, Price Price_, quantity Quantity_)
    : order_type_(Order_type), order_id_(Order_id_), side_(Side_), price_(Price_),
      initial_quantity_(Quantity_), remaining_quantity(Quantity_) {}

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
Price order::get_price() const { return price_; }
ob_order_type order::get_order_type() const { return order_type_; }
quantity order::get_inital_quantity() const { return initial_quantity_; }
quantity order::get_remaining_quantity() const { return remaining_quantity; }
quantity order::get_filled_quantity() const { return initial_quantity_ - remaining_quantity; }
bool order::is_filled() const { return remaining_quantity == 0; }

order_pointer order_modify::to_order_pointer(ob_order_type type) const
{
    // Cold path (modify/replay): heap fallback when no orderbook pool is wired.
    return std::make_shared<order>(type, get_order_id(), get_side(), get_price(), get_quantity());
}

void orderbook::configure_order_pool(ControlBlockPool* cb_pool,
                                     std::size_t min_blocks,
                                     bool forbid_runtime_grow)
{
    order_pool_.set_pool_name("orderbook_order_pool");
    if (cb_pool)
        order_pool_.set_control_block_pool(cb_pool);
    order_pool_.ensure_min_blocks(std::max(min_blocks, std::size_t{1}));
    order_pool_.set_forbid_runtime_grow(forbid_runtime_grow);
    order_pool_ready_ = true;
}

void orderbook::ensure_order_pool_ready()
{
    if (order_pool_ready_)
        return;
    order_pool_.set_pool_name("orderbook_order_pool");
    order_pool_.ensure_min_blocks(1);
    order_pool_ready_ = true;
}

order_pointer orderbook::create_order(ob_order_type type, order_id id, side s,
                                      Price price, quantity qty)
{
    ensure_order_pool_ready();
    return order_pool_.acquire(type, id, s, price, qty);
}

order_id order_modify::get_order_id() const { return orderId_; }
Price order_modify::get_price() const { return price_; }
side order_modify::get_side() const { return side_; }
quantity order_modify::get_quantity() const { return quantity_; }

order_modify::order_modify(order_id order_id, side side, Price price, quantity quantity)
    : orderId_(order_id), price_(price), side_(side), quantity_(quantity) {}

trade::trade(const trade_info& bid_trade, const trade_info& ask_trade)
    : bid_trade_(bid_trade), ask_trade_(ask_trade) {}

const trade_info& trade::get_bid_trade() const { return bid_trade_; }
const trade_info& trade::get_ask_trade() const { return ask_trade_; }

orderbook_lvl_infos::orderbook_lvl_infos(const lvl_infos& bids, const lvl_infos& asks)
    : bids_(bids), asks_(asks) {}

const lvl_infos& orderbook_lvl_infos::get_bids() const { return bids_; }
const lvl_infos& orderbook_lvl_infos::get_asks() const { return asks_; }

order_node* orderbook::alloc_node()
{
    if (!free_nodes_)
    {
        auto blk = std::make_unique<node_block>();
        for (std::size_t i = 0; i < NODE_BLOCK_SIZE; ++i)
        {
            blk->nodes[i].next = free_nodes_;
            free_nodes_ = &blk->nodes[i];
        }
        node_blocks_.push_back(std::move(blk));
    }

    order_node* n = free_nodes_;
    free_nodes_ = n->next;
    n->next = nullptr;
    n->prev = nullptr;
    n->order = nullptr;
    return n;
}

void orderbook::free_node(order_node* n)
{
    n->order.reset();
    n->prev = nullptr;
    n->next = free_nodes_;
    free_nodes_ = n;
}

price_level& orderbook::find_or_insert_level(std::vector<price_level>& levels, Price price, side s)
{
    if (s == side::buy)
    {
        auto it = std::lower_bound(levels.begin(), levels.end(), price,
            [](const price_level& lvl, Price p) { return lvl.price > p; });

        if (it != levels.end() && it->price == price)
            return *it;

        price_level new_level;
        new_level.price = price;
        it = levels.insert(it, new_level);
        return *it;
    }
    else
    {
        auto it = std::lower_bound(levels.begin(), levels.end(), price,
            [](const price_level& lvl, Price p) { return lvl.price < p; });

        if (it != levels.end() && it->price == price)
            return *it;

        price_level new_level;
        new_level.price = price;
        it = levels.insert(it, new_level);
        return *it;
    }
}

void orderbook::remove_level_if_empty(std::vector<price_level>& levels, Price price)
{
    for (auto it = levels.begin(); it != levels.end(); ++it)
    {
        if (it->price == price && it->empty())
        {
            levels.erase(it);
            return;
        }
    }
}

bool orderbook::can_match(side side, Price price) const
{
    if (side == side::buy)
    {
        if (ask_levels_.empty()) return false;
        return price >= ask_levels_.front().price;
    }
    else
    {
        if (bid_levels_.empty()) return false;
        return price <= bid_levels_.front().price;
    }
}

trades orderbook::match_orders()
{
    trades result;

    while (!bid_levels_.empty() && !ask_levels_.empty() &&
           bid_levels_.front().price >= ask_levels_.front().price)
    {
        auto& bid_lvl = bid_levels_.front();
        auto& ask_lvl = ask_levels_.front();

        order_node* bid_node = bid_lvl.head;
        order_node* ask_node = ask_lvl.head;

        if (!bid_node || !ask_node) break;

        if (order_map_.find(bid_node->order->get_order_id()) == order_map_.end())
        {
            bid_lvl.remove(bid_node);
            free_node(bid_node);
            if (bid_lvl.empty()) bid_levels_.erase(bid_levels_.begin());
            continue;
        }
        if (order_map_.find(ask_node->order->get_order_id()) == order_map_.end())
        {
            ask_lvl.remove(ask_node);
            free_node(ask_node);
            if (ask_lvl.empty()) ask_levels_.erase(ask_levels_.begin());
            continue;
        }

        quantity trade_qty = std::min(
            bid_node->order->get_remaining_quantity(),
            ask_node->order->get_remaining_quantity());

        bid_node->order->fill(trade_qty);
        ask_node->order->fill(trade_qty);

        trade_info bid_trade{bid_node->order->get_order_id(), bid_node->order->get_price(), trade_qty};
        trade_info ask_trade{ask_node->order->get_order_id(), ask_node->order->get_price(), trade_qty};
        result.push_back(trade{bid_trade, ask_trade});

        if (bid_node->order->is_filled())
        {
            bid_lvl.remove(bid_node);
            order_map_.erase(bid_node->order->get_order_id());
            free_node(bid_node);
        }
        else
        {
            bid_lvl.total_qty -= trade_qty;
        }

        if (ask_node->order->is_filled())
        {
            ask_lvl.remove(ask_node);
            order_map_.erase(ask_node->order->get_order_id());
            free_node(ask_node);
        }
        else
        {
            ask_lvl.total_qty -= trade_qty;
        }

        if (bid_lvl.empty()) bid_levels_.erase(bid_levels_.begin());
        if (!ask_levels_.empty() && ask_levels_.front().empty())
            ask_levels_.erase(ask_levels_.begin());
    }

    auto cancel_aggressive = [&](std::vector<price_level>& levels) {
        while (!levels.empty())
        {
            auto& lvl = levels.front();
            order_node* n = lvl.head;
            if (!n) { levels.erase(levels.begin()); continue; }

            if (order_map_.find(n->order->get_order_id()) == order_map_.end())
            {
                lvl.remove(n);
                free_node(n);
                if (lvl.empty()) levels.erase(levels.begin());
                continue;
            }

            auto type = n->order->get_order_type();
            if (type == ob_order_type::fill_or_kill || type == ob_order_type::immediate_or_cancel)
            {
                cancel_order(n->order->get_order_id());
                return;
            }
            break;
        }
    };
    cancel_aggressive(bid_levels_);
    cancel_aggressive(ask_levels_);

    return result;
}

trades orderbook::add_order(order_pointer order)
{
    if (order_map_.count(order->get_order_id()))
        return {};

    if (order->get_order_type() == ob_order_type::fill_or_kill)
    {
        if (!can_match(order->get_side(), order->get_price()))
            return {};

        quantity available = 0;
        auto& contra_levels = (order->get_side() == side::buy) ? ask_levels_ : bid_levels_;
        for (const auto& lvl : contra_levels)
        {
            bool crosses = (order->get_side() == side::buy)
                ? lvl.price <= order->get_price()
                : lvl.price >= order->get_price();
            if (crosses)
                available += lvl.total_qty;
        }
        if (available < order->get_remaining_quantity())
            return {};
    }

    if (order->get_order_type() == ob_order_type::immediate_or_cancel &&
        !can_match(order->get_side(), order->get_price()))
    {
        return {};
    }

    order_node* n = alloc_node();
    n->order = order;

    auto& levels = (order->get_side() == side::buy) ? bid_levels_ : ask_levels_;
    auto& lvl = find_or_insert_level(levels, order->get_price(), order->get_side());
    lvl.append(n);

    order_map_[order->get_order_id()] = n;

    return match_orders();
}

void orderbook::cancel_order(order_id oid)
{
    auto it = order_map_.find(oid);
    if (it == order_map_.end())
        return;

    order_node* n = it->second;
    auto s = n->order->get_side();
    auto price = n->order->get_price();

    auto& levels = (s == side::buy) ? bid_levels_ : ask_levels_;

    for (auto& lvl : levels)
    {
        if (lvl.price == price)
        {
            lvl.remove(n);
            break;
        }
    }

    order_map_.erase(it);
    free_node(n);

    remove_level_if_empty(levels, price);
}

trades orderbook::match_order(order_modify order)
{
    if (order_map_.find(order.get_order_id()) == order_map_.end())
        return {};

    auto existing_node = order_map_[order.get_order_id()];
    auto existing_type = existing_node->order->get_order_type();
    cancel_order(order.get_order_id());
    return add_order(order.to_order_pointer(existing_type));
}

bool orderbook::modify_order(order_id id, Price new_price, quantity new_qty)
{
    auto it = order_map_.find(id);
    if (it == order_map_.end())
        return false;

    order_node* n = it->second;
    auto existing_side = n->order->get_side();
    auto existing_type = n->order->get_order_type();

    cancel_order(id);

    auto new_order = create_order(existing_type, id, existing_side,
                                  new_price, new_qty);

    order_node* node = alloc_node();
    node->order = new_order;

    auto& levels = (existing_side == side::buy) ? bid_levels_ : ask_levels_;
    auto& lvl = find_or_insert_level(levels, new_price, existing_side);
    lvl.append(node);

    order_map_[id] = node;
    return true;
}

std::size_t orderbook::size() const
{
    return order_map_.size();
}

orderbook_lvl_infos orderbook::get_order_infos() const
{
    lvl_infos bid_infos, ask_infos;

    for (const auto& lvl : bid_levels_)
        if (lvl.total_qty > 0)
            bid_infos.push_back({lvl.price, lvl.total_qty});

    for (const auto& lvl : ask_levels_)
        if (lvl.total_qty > 0)
            ask_infos.push_back({lvl.price, lvl.total_qty});

    return orderbook_lvl_infos{bid_infos, ask_infos};
}

void orderbook::clear()
{
    for (auto& [id, n] : order_map_)
        free_node(n);

    order_map_.clear();
    bid_levels_.clear();
    ask_levels_.clear();
}

void orderbook::apply_l2_snapshot(const std::pair<Price, quantity>* bids,
                                   std::size_t bid_count,
                                   const std::pair<Price, quantity>* asks,
                                   std::size_t ask_count)
{
    clear();

    for (std::size_t i = 0; i < bid_count; ++i)
    {
        const auto& [p, q] = bids[i];
        if (q == 0) continue;
        auto o = create_order(ob_order_type::good_till_cancel,
                              OrderIdGenerator::next(), side::buy, p, q);
        order_node* n = alloc_node();
        n->order = o;
        auto& lvl = find_or_insert_level(bid_levels_, p, side::buy);
        lvl.append(n);
        order_map_[o->get_order_id()] = n;
    }

    for (std::size_t i = 0; i < ask_count; ++i)
    {
        const auto& [p, q] = asks[i];
        if (q == 0) continue;
        auto o = create_order(ob_order_type::good_till_cancel,
                              OrderIdGenerator::next(), side::sell, p, q);
        order_node* n = alloc_node();
        n->order = o;
        auto& lvl = find_or_insert_level(ask_levels_, p, side::sell);
        lvl.append(n);
        order_map_[o->get_order_id()] = n;
    }
}

void orderbook::apply_l2_update(side side, Price price, quantity new_qty)
{
    auto& levels = (side == side::buy) ? bid_levels_ : ask_levels_;

    for (auto& lvl : levels)
    {
        if (lvl.price == price)
        {
            while (lvl.head)
            {
                order_node* n = lvl.head;
                lvl.remove(n);
                order_map_.erase(n->order->get_order_id());
                free_node(n);
            }
            break;
        }
    }
    remove_level_if_empty(levels, price);

    if (new_qty > 0)
    {
        auto o = create_order(ob_order_type::good_till_cancel,
                              OrderIdGenerator::next(), side, price, new_qty);
        order_node* n = alloc_node();
        n->order = o;
        auto& lvl = find_or_insert_level(levels, price, side);
        lvl.append(n);
        order_map_[o->get_order_id()] = n;
    }
}
