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
quantity order::get_remaining_quantity() const { return remaining_quantity; }
quantity order::get_filled_quantity() const { return initial_quantity_ - remaining_quantity; }
bool order::is_filled() const { return remaining_quantity == 0; }

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
    n->external_l2 = false;
    return n;
}

void orderbook::free_node(order_node* n)
{
    n->order.reset();
    n->prev = nullptr;
    n->external_l2 = false;
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

        // Always adjust total_qty by the trade amount. For fully filled orders,
        // remove() will subtract 0 (post-fill remaining), so the manual subtract
        // ensures correct total even on full fills (fixes previous accounting bug
        // when a level had multiple orders).
        bid_lvl.total_qty -= trade_qty;
        if (bid_node->order->is_filled())
        {
            bid_lvl.remove(bid_node);
            order_map_.erase(bid_node->order->get_order_id());
            free_node(bid_node);
        }

        ask_lvl.total_qty -= trade_qty;
        if (ask_node->order->is_filled())
        {
            ask_lvl.remove(ask_node);
            order_map_.erase(ask_node->order->get_order_id());
            free_node(ask_node);
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

trades orderbook::add_order_impl(order_pointer order, bool external_l2,
                                 bool synthetic_liquidity)
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
    n->external_l2 = external_l2;
    n->synthetic_liquidity = synthetic_liquidity;

    auto& levels = (order->get_side() == side::buy) ? bid_levels_ : ask_levels_;
    auto& lvl = find_or_insert_level(levels, order->get_price(), order->get_side());
    lvl.append(n);

    order_map_[order->get_order_id()] = n;

    return match_orders();
}

trades orderbook::add_order(order_pointer order)
{
    return add_order_impl(std::move(order), false);
}

trades orderbook::add_external_order(order_pointer order)
{
    return add_order_impl(std::move(order), true);
}

trades orderbook::add_synthetic_order(order_pointer order)
{
    return add_order_impl(std::move(order), true, true);
}

trades orderbook::add_order_against_external(order_pointer incoming)
{
    trades result;
    if (!incoming || order_map_.count(incoming->get_order_id()))
        return result;

    auto& contra_levels = incoming->get_side() == side::buy
        ? ask_levels_ : bid_levels_;
    const auto crosses = [&](Price level_price) noexcept {
        return incoming->get_side() == side::buy
            ? level_price <= incoming->get_price()
            : level_price >= incoming->get_price();
    };

    if (incoming->get_order_type() == ob_order_type::fill_or_kill)
    {
        quantity available = 0;
        const quantity required = incoming->get_remaining_quantity();
        for (const auto& level : contra_levels)
        {
            if (!crosses(level.price))
                break;
            for (auto* node = level.head; node; node = node->next)
            {
                if (!node->external_l2)
                    continue;
                const quantity remaining =
                    node->order->get_remaining_quantity();
                if (remaining >= required - available)
                {
                    available = required;
                    break;
                }
                available += remaining;
            }
            if (available == required)
                break;
        }
        if (available < required)
            return result;
    }

    for (auto level_it = contra_levels.begin();
         level_it != contra_levels.end()
             && incoming->get_remaining_quantity() > 0; )
    {
        if (!crosses(level_it->price))
            break;

        auto* node = level_it->head;
        while (node && incoming->get_remaining_quantity() > 0)
        {
            auto* next = node->next;
            if (!node->external_l2)
            {
                node = next;
                continue;
            }

            const quantity trade_qty = std::min(
                incoming->get_remaining_quantity(),
                node->order->get_remaining_quantity());
            incoming->fill(trade_qty);
            node->order->fill(trade_qty);
            level_it->total_qty -= trade_qty;

            if (incoming->get_side() == side::buy)
            {
                result.emplace_back(
                    trade_info{incoming->get_order_id(),
                               incoming->get_price(), trade_qty},
                    trade_info{node->order->get_order_id(),
                               node->order->get_price(), trade_qty});
            }
            else
            {
                result.emplace_back(
                    trade_info{node->order->get_order_id(),
                               node->order->get_price(), trade_qty},
                    trade_info{incoming->get_order_id(),
                               incoming->get_price(), trade_qty});
            }

            if (node->order->is_filled())
            {
                const auto id = node->order->get_order_id();
                level_it->remove(node); // post-fill remaining is zero
                order_map_.erase(id);
                free_node(node);
            }
            node = next;
        }

        if (level_it->empty())
            level_it = contra_levels.erase(level_it);
        else
            ++level_it;
    }

    if (incoming->get_remaining_quantity() > 0
        && incoming->get_order_type() == ob_order_type::good_till_cancel)
    {
        order_node* node = alloc_node();
        node->order = incoming;
        auto& own_levels = incoming->get_side() == side::buy
            ? bid_levels_ : ask_levels_;
        auto& level = find_or_insert_level(
            own_levels, incoming->get_price(), incoming->get_side());
        level.append(node);
        order_map_[incoming->get_order_id()] = node;
    }

    return result;
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

order_pointer orderbook::get_order(order_id id) const
{
    auto it = order_map_.find(id);
    if (it == order_map_.end() || !it->second)
        return nullptr;
    return it->second->order;
}

std::size_t orderbook::size() const
{
    return order_map_.size();
}

namespace {
double best_external_price(const std::vector<price_level>& levels) noexcept
{
    for (const auto& level : levels)
    {
        for (auto* node = level.head; node; node = node->next)
            if (node->external_l2 && !node->synthetic_liquidity)
                return level.price.to_double();
    }
    return 0.0;
}
}

double orderbook::best_external_bid_price() const noexcept
{
    return best_external_price(bid_levels_);
}

double orderbook::best_external_ask_price() const noexcept
{
    return best_external_price(ask_levels_);
}

std::pair<Price, quantity> orderbook::best_external_level(
    const std::vector<price_level>& levels) noexcept
{
    for (const auto& level : levels)
    {
        quantity total = 0;
        bool has_external = false;
        for (auto* node = level.head; node; node = node->next)
        {
            if (!node->external_l2 || node->synthetic_liquidity)
                continue;
            const quantity remaining = node->order->get_remaining_quantity();
            if (std::numeric_limits<quantity>::max() - total < remaining)
                return {level.price, std::numeric_limits<quantity>::max()};
            total += remaining;
            has_external = true;
        }
        if (has_external && total > 0)
            return {level.price, total};
    }
    return {};
}

external_bbo orderbook::best_external_bbo() const noexcept
{
    const auto [bid_price, bid_quantity] = best_external_level(bid_levels_);
    const auto [ask_price, ask_quantity] = best_external_level(ask_levels_);
    return {bid_price, bid_quantity, ask_price, ask_quantity};
}

double orderbook::external_vwap(side taker_side,
                                quantity requested) const noexcept
{
    if (requested == 0)
        return 0.0;
    const auto& levels = taker_side == side::buy
        ? ask_levels_ : bid_levels_;
    quantity remaining = requested;
    long double cost = 0.0L;
    for (const auto& level : levels)
    {
        for (auto* node = level.head; node && remaining > 0;
             node = node->next)
        {
            if (!node->external_l2 || node->synthetic_liquidity)
                continue;
            const quantity take = std::min(
                remaining, node->order->get_remaining_quantity());
            cost += static_cast<long double>(take)
                * static_cast<long double>(level.price.to_double());
            remaining -= take;
        }
        if (remaining == 0)
            break;
    }
    if (remaining != 0)
        return 0.0;
    return static_cast<double>(cost / static_cast<long double>(requested));
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
    clear_external_l2();

    for (std::size_t i = 0; i < bid_count; ++i)
    {
        const auto& [p, q] = bids[i];
        if (q == 0) continue;
        auto o = create_order(ob_order_type::good_till_cancel,
                              OrderIdGenerator::next(), side::buy, p, q);
        order_node* n = alloc_node();
        n->order = o;
        n->external_l2 = true;
        n->synthetic_liquidity = false;
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
        n->external_l2 = true;
        n->synthetic_liquidity = false;
        auto& lvl = find_or_insert_level(ask_levels_, p, side::sell);
        lvl.append(n);
        order_map_[o->get_order_id()] = n;
    }
}

void orderbook::apply_l2_update(side side, Price price, quantity new_qty)
{
    auto& levels = (side == side::buy) ? bid_levels_ : ask_levels_;
    clear_external_l2_at(levels, price);

    if (new_qty > 0)
    {
        auto o = create_order(ob_order_type::good_till_cancel,
                              OrderIdGenerator::next(), side, price, new_qty);
        order_node* n = alloc_node();
        n->order = o;
        n->external_l2 = true;
        n->synthetic_liquidity = false;
        auto& lvl = find_or_insert_level(levels, price, side);
        lvl.append(n);
        order_map_[o->get_order_id()] = n;
    }
}

void orderbook::clear_external_l2_at(std::vector<price_level>& levels,
                                     Price price)
{
    for (auto level_it = levels.begin(); level_it != levels.end(); ++level_it)
    {
        if (level_it->price != price)
            continue;
        auto* node = level_it->head;
        while (node)
        {
            auto* next = node->next;
            if (node->external_l2 && !node->synthetic_liquidity)
            {
                const auto id = node->order->get_order_id();
                level_it->remove(node);
                order_map_.erase(id);
                free_node(node);
            }
            node = next;
        }
        if (level_it->empty())
            levels.erase(level_it);
        return;
    }
}

void orderbook::clear_external_l2()
{
    auto clear_side = [&](std::vector<price_level>& levels) {
        for (auto level_it = levels.begin(); level_it != levels.end(); )
        {
            auto* node = level_it->head;
            while (node)
            {
                auto* next = node->next;
                if (node->external_l2 && !node->synthetic_liquidity)
                {
                    const auto id = node->order->get_order_id();
                    level_it->remove(node);
                    order_map_.erase(id);
                    free_node(node);
                }
                node = next;
            }
            if (level_it->empty())
                level_it = levels.erase(level_it);
            else
                ++level_it;
        }
    };
    clear_side(bid_levels_);
    clear_side(ask_levels_);
}
