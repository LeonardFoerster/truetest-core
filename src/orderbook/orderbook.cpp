#include "orderbook.h"
#include "../types/control_block_pool.h"
#include "../types/order_id.h"
#include <stdexcept>
#include <algorithm>
#include <cassert>
#include <exception>
#include <limits>
#include <utility>

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

orderbook_lvl_infos::orderbook_lvl_infos(lvl_infos bids, lvl_infos asks)
    : bids_(std::move(bids)), asks_(std::move(asks)) {}

const lvl_infos& orderbook_lvl_infos::get_bids() const { return bids_; }
const lvl_infos& orderbook_lvl_infos::get_asks() const { return asks_; }

order_node* orderbook::alloc_node()
{
    if (!free_nodes_)
    {
        auto blk = std::make_unique<node_block>();
        // Publish ownership before exposing any address from the block. If
        // vector growth throws, free_nodes_ must not point into `blk` after
        // its destructor releases the storage.
        node_blocks_.push_back(std::move(blk));
        auto* owned = node_blocks_.back().get();
        for (std::size_t i = 0; i < NODE_BLOCK_SIZE; ++i)
        {
            owned->nodes[i].next = free_nodes_;
            free_nodes_ = &owned->nodes[i];
        }
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

order_id orderbook::next_unused_l2_order_id() noexcept
{
    constexpr order_id internal_bit = order_id{1} << 63;
    constexpr order_id sequence_mask = internal_bit - 1;
    for (;;)
    {
        const order_id id = next_l2_order_id_;
        const order_id next_sequence = ((id & sequence_mask) + 1)
            & sequence_mask;
        next_l2_order_id_ = internal_bit
            | (next_sequence == 0 ? order_id{1} : next_sequence);
        if (!order_map_.contains(id))
            return id;
    }
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

trades orderbook::add_order_impl(order_pointer order, bool external_l2)
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
            {
                const quantity required = order->get_remaining_quantity();
                if (lvl.total_qty >= required - available)
                {
                    available = required;
                    break;
                }
                available += lvl.total_qty;
            }
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

    auto& levels = (order->get_side() == side::buy) ? bid_levels_ : ask_levels_;
    auto& lvl = find_or_insert_level(levels, order->get_price(), order->get_side());
    if (!lvl.append(n))
    {
        free_node(n);
        remove_level_if_empty(levels, order->get_price());
        return {};
    }

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

    quantity predicted_remainder = incoming->get_remaining_quantity();
    if (incoming->get_order_type() == ob_order_type::good_till_cancel
        || incoming->get_order_type() == ob_order_type::fill_or_kill)
    {
        for (const auto& level : contra_levels)
        {
            if (!crosses(level.price))
                break;
            for (auto* node = level.head;
                 node && predicted_remainder > 0; node = node->next)
            {
                if (!node->external_l2)
                    continue;
                predicted_remainder -= std::min(
                    predicted_remainder,
                    node->order->get_remaining_quantity());
            }
            if (predicted_remainder == 0)
                break;
        }
    }

    if (incoming->get_order_type() == ob_order_type::good_till_cancel
        && predicted_remainder > 0)
    {
        const auto& own_levels = incoming->get_side() == side::buy
            ? bid_levels_ : ask_levels_;
        for (const auto& level : own_levels)
        {
            if (level.price != incoming->get_price()) continue;
            if (predicted_remainder >
                std::numeric_limits<quantity>::max() - level.total_qty)
                return result;
            break;
        }
    }

    if (incoming->get_order_type() == ob_order_type::fill_or_kill)
    {
        if (predicted_remainder != 0)
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
        if (level.append(node))
            order_map_[incoming->get_order_id()] = node;
        else
        {
            free_node(node);
            remove_level_if_empty(own_levels, incoming->get_price());
        }
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
    const auto old_price = n->order->get_price();
    const auto old_qty = n->order->get_remaining_quantity();

    // Validate before cancelling so a rejected amend preserves the original
    // resting order (strong guarantee on this single-writer book).
    auto& target_levels = existing_side == side::buy ? bid_levels_ : ask_levels_;
    for (const auto& level : target_levels)
    {
        if (level.price != new_price) continue;
        quantity base = level.total_qty;
        if (new_price == old_price)
            base -= old_qty;
        if (new_qty > std::numeric_limits<quantity>::max() - base)
            return false;
        break;
    }

    // Every operation that may allocate or throw happens before the original
    // node is detached. Pool exhaustion, bad_alloc, and vector growth thus
    // leave both the map entry and price-level chain untouched.
    auto new_order = create_order(existing_type, id, existing_side,
                                  new_price, new_qty);
    order_node* replacement = alloc_node();
    replacement->order = std::move(new_order);
    replacement->external_l2 = n->external_l2;

    auto& levels = (existing_side == side::buy) ? bid_levels_ : ask_levels_;
    price_level* target = nullptr;
    try
    {
        target = &find_or_insert_level(levels, new_price, existing_side);
    }
    catch (...)
    {
        free_node(replacement);
        throw;
    }

    price_level* original_level = nullptr;
    for (auto& level : levels)
    {
        if (level.price == old_price)
        {
            original_level = &level;
            break;
        }
    }
    assert(original_level != nullptr);

    if (target != original_level)
    {
        // The checked-capacity precondition makes this append infallible. Do
        // it before detaching the old node so even a violated invariant keeps
        // the original order live.
        if (!target->append(replacement))
        {
            free_node(replacement);
            remove_level_if_empty(levels, new_price);
            return false;
        }
        original_level->remove(n);
    }
    else
    {
        // At the same price the replacement capacity was computed excluding
        // the old quantity, so detach then append. Both operations are
        // noexcept and the arithmetic was proven safe above.
        original_level->remove(n);
        const bool appended = original_level->append(replacement);
        assert(appended);
        if (!appended)
        {
            const bool restored = original_level->append(n);
            assert(restored);
            (void)restored;
            free_node(replacement);
            return false;
        }
    }

    it->second = replacement;
    free_node(n);
    if (target != original_level)
        remove_level_if_empty(levels, old_price);
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
            if (node->external_l2)
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
            if (!node->external_l2)
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

    return orderbook_lvl_infos{std::move(bid_infos), std::move(ask_infos)};
}

orderbook_lvl_infos orderbook::get_external_order_infos() const
{
    const auto external_levels = [](const std::vector<price_level>& levels) {
        lvl_infos result;
        result.reserve(levels.size());
        for (const auto& level : levels)
        {
            quantity total = 0;
            for (auto* node = level.head; node; node = node->next)
                if (node->external_l2)
                {
                    const auto remaining = node->order->get_remaining_quantity();
                    if (remaining > std::numeric_limits<quantity>::max() - total)
                    {
                        total = 0;
                        break;
                    }
                    total += remaining;
                }
            if (total > 0)
                result.push_back({level.price, total});
        }
        return result;
    };
    return {external_levels(bid_levels_), external_levels(ask_levels_)};
}

external_depth_result orderbook::copy_external_depth(
    std::span<lvl_info> bids, std::span<lvl_info> asks) const noexcept
{
    external_depth_result result;
    const auto copy_side = [&](const std::vector<price_level>& levels,
                               std::span<lvl_info> output,
                               std::size_t& copied,
                               std::size_t& total_levels) {
        for (const auto& level : levels)
        {
            quantity total = 0;
            for (auto* node = level.head; node; node = node->next)
            {
                if (!node->external_l2) continue;
                const auto remaining = node->order->get_remaining_quantity();
                if (remaining > std::numeric_limits<quantity>::max() - total)
                {
                    result.quantity_overflow = true;
                    return;
                }
                total += remaining;
            }
            if (total == 0) continue;
            ++total_levels;
            if (copied < output.size())
                output[copied++] = {level.price, total};
        }
    };

    copy_side(bid_levels_, bids, result.bid_count, result.total_bid_levels);
    if (!result.quantity_overflow)
        copy_side(ask_levels_, asks, result.ask_count, result.total_ask_levels);
    return result;
}

void orderbook::clear()
{
    for (auto& [id, n] : order_map_)
        free_node(n);

    order_map_.clear();
    bid_levels_.clear();
    ask_levels_.clear();
}

namespace {
using l2_level = std::pair<Price, quantity>;

bool add_quantity(quantity value, quantity& total) noexcept
{
    if (value > std::numeric_limits<quantity>::max() - total)
        return false;
    total += value;
    return true;
}

bool external_quantity_fits(Price price, quantity external_qty,
                            const std::vector<price_level>& existing) noexcept
{
    quantity local_qty = 0;
    for (const auto& level : existing)
    {
        if (level.price != price)
            continue;
        for (auto* node = level.head; node; node = node->next)
        {
            if (!node->external_l2
                && !add_quantity(
                    node->order->get_remaining_quantity(), local_qty))
                return false;
        }
        break;
    }
    return external_qty <=
        std::numeric_limits<quantity>::max() - local_qty;
}

bool validate_l2_levels(const l2_level* input, std::size_t count,
                        const std::vector<price_level>& existing,
                        std::size_t& unique_levels)
{
    if (count != 0 && input == nullptr)
        throw std::invalid_argument("null L2 levels with non-zero count");

    unique_levels = 0;
    bool nondecreasing = true;
    bool nonincreasing = true;
    const l2_level* previous = nullptr;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (input[i].second == 0)
            continue;
        if (previous)
        {
            nondecreasing = nondecreasing
                && previous->first <= input[i].first;
            nonincreasing = nonincreasing
                && previous->first >= input[i].first;
        }
        previous = &input[i];
    }

    if (nondecreasing || nonincreasing)
    {
        std::size_t i = 0;
        while (i < count)
        {
            if (input[i].second == 0)
            {
                ++i;
                continue;
            }
            const Price price = input[i].first;
            quantity external_qty = 0;
            while (i < count)
            {
                if (input[i].second == 0)
                {
                    ++i;
                    continue;
                }
                if (input[i].first != price)
                    break;
                if (!add_quantity(input[i].second, external_qty))
                    return false;
                ++i;
            }
            ++unique_levels;
            if (!external_quantity_fits(price, external_qty, existing))
                return false;
        }
        return true;
    }

    // Provider snapshots are normally monotonic, making the path above O(n).
    // Retain correctness for malformed/unsorted input without allocating a
    // hash table on the event path.
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto& [price, qty] = input[i];
        if (qty == 0)
            continue;
        bool already_seen = false;
        for (std::size_t j = 0; j < i; ++j)
            if (input[j].second != 0 && input[j].first == price)
            {
                already_seen = true;
                break;
            }
        if (already_seen)
            continue;

        quantity external_qty = 0;
        for (std::size_t j = i; j < count; ++j)
            if (input[j].second != 0 && input[j].first == price
                && !add_quantity(input[j].second, external_qty))
                return false;
        ++unique_levels;
        if (!external_quantity_fits(price, external_qty, existing))
            return false;
    }
    return true;
}

bool local_quantity_at(const std::vector<price_level>& levels, Price price,
                       quantity& local_qty) noexcept
{
    local_qty = 0;
    for (const auto& level : levels)
    {
        if (level.price != price)
            continue;
        for (auto* node = level.head; node; node = node->next)
        {
            if (!node->external_l2
                && !add_quantity(
                    node->order->get_remaining_quantity(), local_qty))
                return false;
        }
        break;
    }
    return true;
}
}

l2_apply_status orderbook::apply_l2_snapshot(
    const std::pair<Price, quantity>* bids, std::size_t bid_count,
    const std::pair<Price, quantity>* asks, std::size_t ask_count)
{
    std::size_t unique_bid_levels = 0;
    std::size_t unique_ask_levels = 0;
    if (!validate_l2_levels(
            bids, bid_count, bid_levels_, unique_bid_levels)
        || !validate_l2_levels(
            asks, ask_count, ask_levels_, unique_ask_levels))
        return l2_apply_status::quantity_overflow;

    if (ask_count > std::numeric_limits<std::size_t>::max() - bid_count)
        throw std::length_error("L2 snapshot level count overflow");

    order_node* staged_head = nullptr;
    order_node* staged_tail = nullptr;
    std::size_t staged_count = 0;
    std::size_t registered = 0;

    const auto release_staged = [&]() noexcept {
        auto* node = staged_head;
        while (node)
        {
            auto* next = node->next;
            free_node(node);
            node = next;
        }
    };
    const auto erase_registered = [&]() noexcept {
        auto* node = staged_head;
        for (std::size_t i = 0; i < registered; ++i)
        {
            auto* next = node->next;
            order_map_.erase(node->order->get_order_id());
            node = next;
        }
    };
    const auto stage_side = [&](const l2_level* input, std::size_t count,
                                side book_side) {
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& [price, qty] = input[i];
            if (qty == 0)
                continue;
            const order_id id = next_unused_l2_order_id();
            auto staged_order = create_order(
                ob_order_type::good_till_cancel, id, book_side, price, qty);
            order_node* node = alloc_node();
            node->order = std::move(staged_order);
            node->external_l2 = true;
            if (staged_tail)
                staged_tail->next = node;
            else
                staged_head = node;
            staged_tail = node;
            ++staged_count;
        }
    };

    try
    {
        stage_side(bids, bid_count, side::buy);
        stage_side(asks, ask_count, side::sell);

        // Reserve every container before registering staged nodes. From the
        // first external-depth removal onward, the commit is allocation-free.
        if (unique_bid_levels >
                std::numeric_limits<std::size_t>::max()
                    - bid_levels_.size()
            || unique_ask_levels >
                std::numeric_limits<std::size_t>::max()
                    - ask_levels_.size())
            throw std::length_error("L2 snapshot price-level count overflow");
        bid_levels_.reserve(bid_levels_.size() + unique_bid_levels);
        ask_levels_.reserve(ask_levels_.size() + unique_ask_levels);
        if (staged_count >
            std::numeric_limits<std::size_t>::max() - order_map_.size())
            throw std::length_error("L2 snapshot order count overflow");
        order_map_.reserve(order_map_.size() + staged_count);
        for (auto* node = staged_head; node; node = node->next)
        {
            const auto [ignored, inserted] =
                order_map_.emplace(node->order->get_order_id(), node);
            (void)ignored;
            if (!inserted)
                throw std::logic_error("generated duplicate L2 order id");
            ++registered;
        }
    }
    catch (...)
    {
        erase_registered();
        release_staged();
        throw;
    }

    clear_external_l2();

    auto* node = staged_head;
    while (node)
    {
        auto* next = node->next;
        node->next = nullptr;
        const auto book_side = node->order->get_side();
        const auto price = node->order->get_price();
        auto& levels = book_side == side::buy
            ? bid_levels_ : ask_levels_;
        auto& level = find_or_insert_level(
            levels, price, book_side);
        // validate_l2_levels proved the complete per-price quantity fits,
        // including any locally resting quantity preserved by replacement.
        const bool appended = level.append(node);
        assert(appended);
        if (!appended)
            std::terminate();
        node = next;
    }

    return l2_apply_status::applied;
}

l2_apply_status orderbook::apply_l2_update(side book_side, Price price,
                                            quantity new_qty)
{
    auto& levels = book_side == side::buy ? bid_levels_ : ask_levels_;
    quantity local_qty = 0;
    if (!local_quantity_at(levels, price, local_qty)
        || new_qty > std::numeric_limits<quantity>::max() - local_qty)
        return l2_apply_status::quantity_overflow;

    if (new_qty == 0)
    {
        clear_external_l2_at(levels, price);
        return l2_apply_status::applied;
    }

    const order_id id = next_unused_l2_order_id();
    auto staged_order = create_order(
        ob_order_type::good_till_cancel, id, book_side, price, new_qty);
    order_node* staged = alloc_node();
    staged->order = std::move(staged_order);
    staged->external_l2 = true;

    bool registered = false;
    try
    {
        // These are the only potentially allocating structural operations;
        // both precede removal of the currently published venue level.
        const bool price_level_exists = std::any_of(
            levels.begin(), levels.end(),
            [price](const price_level& level) {
                return level.price == price;
            });
        if (!price_level_exists && levels.size() == levels.capacity())
        {
            const std::size_t current = levels.size();
            const std::size_t maximum = levels.max_size();
            if (current == maximum)
                throw std::length_error("L2 update price-level count overflow");
            const std::size_t grown = current == 0
                ? 1
                : current > maximum / 2 ? maximum : current * 2;
            levels.reserve(grown);
        }
        if (order_map_.size() ==
            std::numeric_limits<std::size_t>::max())
            throw std::length_error("L2 update order count overflow");
        order_map_.reserve(order_map_.size() + 1);
        const auto [ignored, inserted] = order_map_.emplace(id, staged);
        (void)ignored;
        if (!inserted)
            throw std::logic_error("generated duplicate L2 order id");
        registered = true;
    }
    catch (...)
    {
        if (registered)
            order_map_.erase(id);
        free_node(staged);
        throw;
    }

    clear_external_l2_at(levels, price);
    auto& level = find_or_insert_level(levels, price, book_side);
    const bool appended = level.append(staged);
    assert(appended);
    if (!appended)
        std::terminate();
    return l2_apply_status::applied;
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
            if (node->external_l2)
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
                if (node->external_l2)
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
