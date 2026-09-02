// Engine market-data path: bar/tick/L2 application, per-event dispatch to
// strategies/exits, mark-price bookkeeping, and the run()/run_streaming()
// family's event-loop pumps (bar, tick, replay, unified provider streaming).
// Extracted mechanically from engine.cpp (Phase 1 TU split); behavior unchanged.
#include "engine.h"
#include "data/quantity_scale.h"
#include "live_safety_session.h"
#include "data/date_parse.h"
#include "execution/queue_aware_book_adapter.h"
#include "providers/provider.h"
#include "ui/console_dashboard.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

void engine::apply_l2_snapshot(const std::string& symbol,
                               const std::vector<l2_level>& bids,
                               const std::vector<l2_level>& asks,
                               std::chrono::system_clock::time_point timestamp,
                               std::uint64_t quantity_scale,
                               std::uint64_t last_update_id)
{
    drain_object_pool_returns();

    if (symbol.empty())
    {
        trigger_halt("invalid L2 snapshot symbol");
        return;
    }

    const std::size_t n_bids =
        std::min(bids.size(), kL2SnapshotMaxLevels);
    const std::size_t n_asks =
        std::min(asks.size(), kL2SnapshotMaxLevels);

    std::array<std::pair<Price, quantity>, kL2SnapshotMaxLevels> ob_bids{};
    std::array<std::pair<Price, quantity>, kL2SnapshotMaxLevels> ob_asks{};
    std::array<std::pair<double, double>, kL2SnapshotMaxLevels> abids{};
    std::array<std::pair<double, double>, kL2SnapshotMaxLevels> aasks{};

    for (std::size_t i = 0; i < n_bids; ++i)
    {
        Price book_price;
        if (bids[i].price <= 0.0
            || !Price::try_from_double(bids[i].price, book_price))
        {
            trigger_halt("invalid L2 snapshot bid price");
            return;
        }
        std::uint64_t book_qty = 0;
        if (!tt::quantity_scale::rescale_nonnegative(
                bids[i].quantity, quantity_scale, config_.qty_scale, book_qty))
        {
            trigger_halt("invalid L2 snapshot bid quantity or scale");
            return;
        }
        ob_bids[i] = {book_price,
                      static_cast<quantity>(book_qty)};
        abids[i] = {bids[i].price,
                    tt::quantity_scale::to_base(bids[i].quantity,
                                                quantity_scale)};
    }
    for (std::size_t i = 0; i < n_asks; ++i)
    {
        Price book_price;
        if (asks[i].price <= 0.0
            || !Price::try_from_double(asks[i].price, book_price))
        {
            trigger_halt("invalid L2 snapshot ask price");
            return;
        }
        std::uint64_t book_qty = 0;
        if (!tt::quantity_scale::rescale_nonnegative(
                asks[i].quantity, quantity_scale, config_.qty_scale, book_qty))
        {
            trigger_halt("invalid L2 snapshot ask quantity or scale");
            return;
        }
        ob_asks[i] = {book_price,
                      static_cast<quantity>(book_qty)};
        aasks[i] = {asks[i].price,
                    tt::quantity_scale::to_base(asks[i].quantity,
                                                quantity_scale)};
    }

    // Validate the complete snapshot before mutating either the seeded-symbol
    // set or the book. A single corrupt quantity must not clear/partially
    // replace previously valid depth.
    l2_seeded_symbols_.insert(symbol);
    auto ob = orderbook_registry_.get_or_create(symbol);
    ob->apply_l2_snapshot(ob_bids.data(), n_bids, ob_asks.data(), n_asks);
    const std::uint16_t sequence_symbol_id =
        orderbook_registry_.symbol_table().id_of(symbol);
    if (sequence_symbol_id == SymbolTable::kInvalidId)
    {
        trigger_halt("L2 snapshot symbol was not interned");
        return;
    }
    if (last_update_id != 0)
        l2_sequence_states_[sequence_symbol_id] = {
            last_update_id, true, true};
    else
        l2_sequence_states_[sequence_symbol_id] = {};
    const double best_bid = ob->best_external_bid_price();
    const double best_ask = ob->best_external_ask_price();
    const double l2_mark = (best_bid > 0.0 && best_ask > 0.0)
        ? (best_bid + best_ask) * 0.5
        : (best_bid > 0.0 ? best_bid : best_ask);
    if (l2_mark > 0.0)
    {
        last_mid_price_.store(l2_mark, std::memory_order_release);
        last_mark_symbol_ = symbol;
        std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
        last_mark_prices_[symbol] = mark_point{l2_mark, timestamp};
    }
    else
    {
        std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
        last_mark_prices_.erase(symbol);
        if (last_mark_symbol_ == symbol)
            last_mark_symbol_.clear();
    }
    refresh_top_of_book_atomics(*ob);

    // Forward L2 to execution adapters so QueueAwareBookAdapter (paper) and
    // TradeTapeShadowAdapter (shadow queue model) can maintain level aggregates
    // / queue_ahead. Central place for all L2-driven paths (direct apply, replay,
    // streaming). Duplicated in provider event dispatch for the live shadow_exec
    // case; keep in sync.
    l2_bid_scratch_.assign(abids.begin(), abids.begin() + n_bids);
    l2_ask_scratch_.assign(aasks.begin(), aasks.begin() + n_asks);
    // Historical L2 must retain capture time. A missing timestamp is not
    // replaced by wall clock: queue freshness would become nondeterministic.
    const auto l2_ts = timestamp;
    // Public/stream dispatch cannot reach this method until construction has
    // completed. Keep Authority sequenced L2 timestamps; fail-loud instead of
    // silently skipping queue updates / advance_all on a constructed engine.
    if (!router_ || !orders_)
    {
        trigger_halt("L2 snapshot dispatch before engine construction completed");
        return;
    }
    router_->on_l2_snapshot(
        symbol, l2_bid_scratch_, l2_ask_scratch_, l2_ts);
    last_sim_time_ = l2_ts;
    last_decision_ts_ = l2_ts;   // F-08: point observation
    auto ev = acquire_pooled(l2_snapshot_pool_, l2_ts, symbol,
        bids.data(), n_bids, asks.data(), n_asks, quantity_scale);
    const int64_t l2_recv_ns = std::chrono::duration_cast<
        std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    ev->set_recv_ns(l2_recv_ns);
    log_event(*ev);
    publish_event(ev);
    // The engine-owned Analytics instance is the synchronous pre-trade risk
    // view in every preset. Keep both spread and marked equity current here;
    // threaded worker Analytics receives its independent copy via publish.
    analytics_.on_event(ev);

    if (pause_all_.load(std::memory_order_acquire)
        || halt_flag_.load(std::memory_order_acquire))
        return;
    if (!(l2_mark > 0.0))
        return;

    // A snapshot is a same-symbol market observation just like an
    // incremental depth update. It advances latency clocks and releases
    // event-count-delayed orders, but does not invent a strategy callback
    // because IStrategy exposes only on_l2_update.
    std::size_t l2_event_count = 0;
    bool l2_halt = false;
    router_->advance_all(l2_ts);
    orders_->drain_due(l2_ts, l2_event_count, l2_halt, symbol);
    if (l2_halt || halt_flag_.load(std::memory_order_acquire))
        return;
    if (l2_mark > 0.0)
        (void)orders_->evaluate_exits(symbol, l2_mark, l2_ts,
                             l2_event_count, l2_recv_ns);
}

void engine::apply_l2_update(const std::string& symbol,
                             tick_side ts_side, double price, int64_t new_qty,
                             std::chrono::system_clock::time_point timestamp,
                             std::uint64_t quantity_scale)
{
    drain_object_pool_returns();

    Price book_price;
    if (symbol.empty() || price <= 0.0
        || !Price::try_from_double(price, book_price)
        || (ts_side != tick_side::bid && ts_side != tick_side::ask))
    {
        trigger_halt("invalid L2 update symbol, side, or price");
        return;
    }

    side ob_side = (ts_side == tick_side::bid) ? side::buy : side::sell;
    std::uint64_t book_qty = 0;
    if (!tt::quantity_scale::rescale_nonnegative(
            new_qty, quantity_scale, config_.qty_scale, book_qty))
    {
        trigger_halt("invalid L2 update quantity or scale");
        return;
    }
    auto ob = orderbook_registry_.get_or_create(symbol);
    ob->apply_l2_update(ob_side, book_price,
                        static_cast<quantity>(book_qty));
    const double best_bid = ob->best_external_bid_price();
    const double best_ask = ob->best_external_ask_price();
    const double l2_mark = (best_bid > 0.0 && best_ask > 0.0)
        ? (best_bid + best_ask) * 0.5
        : (best_bid > 0.0 ? best_bid : best_ask);
    if (l2_mark > 0.0)
    {
        last_mid_price_.store(l2_mark, std::memory_order_release);
        last_mark_symbol_ = symbol;
        std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
        last_mark_prices_[symbol] = mark_point{l2_mark, timestamp};
    }
    else
    {
        std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
        last_mark_prices_.erase(symbol);
        if (last_mark_symbol_ == symbol)
            last_mark_symbol_.clear();
    }
    refresh_top_of_book_atomics(*ob);

    // Forward L2 update to adapters for queue models (see apply_l2_snapshot).
    const order_side os = (ts_side == tick_side::bid) ? order_side::buy : order_side::sell;
    // See snapshot path: no wall-clock substitution for replay records.
    const auto l2_ts = timestamp;
    // Same post-construction invariant as apply_l2_snapshot: sequenced
    // timestamps stay; a missing router/orders_ is a construction bug, not a
    // skip of advance_all.
    if (!router_ || !orders_)
    {
        trigger_halt("L2 update dispatch before engine construction completed");
        return;
    }
    router_->on_l2_update(
        symbol, os, price,
        tt::quantity_scale::to_base(new_qty, quantity_scale), l2_ts);
    last_sim_time_ = l2_ts;
    last_decision_ts_ = l2_ts;   // F-08: point observation
    const int64_t l2_recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto ev = acquire_pooled(l2_update_pool_,
        l2_ts, symbol, ts_side, price, new_qty, quantity_scale);
    ev->set_recv_ns(l2_recv_ns);
    log_event(*ev);
    publish_event(ev);
    if (l2_mark > 0.0)
        analytics_.on_mark(symbol, l2_mark);

    /* LIVE_SAFETY_CCB_APPROVED: L2 strategy dispatch after apply + publish
       (same thread as on_tick/on_market). Halt-gated per strategy; exit
       evaluation + exit intents mirror the tick path. */
    if (pause_all_.load(std::memory_order_acquire) ||
        halt_flag_.load(std::memory_order_acquire))
        return;

    size_t l2_event_count = 0;
    bool l2_halt = false;

    // Drain pending orders eligible at this L2 timestamp. Default
    // Bar delay counts later same-symbol observations; without this drain,
    // pure L2 streams would never release their queued strategy orders.
    router_->advance_all(l2_ts);
    if (l2_mark > 0.0)
        orders_->drain_due(l2_ts, l2_event_count, l2_halt, symbol);
    if (l2_halt || halt_flag_.load(std::memory_order_acquire))
        return;

    // Mid/last price for ExitManager on pure L2 streams (no tick/bar).
    if (l2_mark > 0.0
        && orders_->evaluate_exits(symbol, l2_mark, l2_ts,
                          l2_event_count, l2_recv_ns))
        return;

    if (strategy_) {
        sync_strategy_account_equity(*strategy_);
        if (auto o = strategy_->on_l2_update(*ev)) {
            o->set_recv_ns(l2_recv_ns);
            o->set_strategy_name(primary_strategy_name_);
            const double pre_net = !primary_strategy_name_.empty()
                ? portfolio_.get_strategy_position_qty(primary_strategy_name_, o->get_symbol())
                : (portfolio_.get_positions().count(o->get_symbol()) ? portfolio_.get_positions().at(o->get_symbol()).qty : 0.0);
            bool route_halt = false;
            orders_->route(*o, l2_ts, l2_event_count, route_halt);
            orders_->finalize_route(*strategy_, primary_strategy_name_, *o, route_halt, pre_net);
            if (route_halt || halt_flag_.load(std::memory_order_acquire))
                return;
        }
    }
    for (std::size_t i = 0; i < additional_strategies_.size(); ++i) {
        if (halt_flag_.load(std::memory_order_acquire))
            return;
        auto& s = additional_strategies_[i];
        if (!s) continue;
        sync_strategy_account_equity(*s);
        if (auto o = s->on_l2_update(*ev)) {
            o->set_recv_ns(l2_recv_ns);
            o->set_strategy_name(additional_strategy_names_[i]);
            const double pre_net = !additional_strategy_names_[i].empty()
                ? portfolio_.get_strategy_position_qty(additional_strategy_names_[i], o->get_symbol())
                : (portfolio_.get_positions().count(o->get_symbol()) ? portfolio_.get_positions().at(o->get_symbol()).qty : 0.0);
            bool route_halt = false;
            orders_->route(*o, l2_ts, l2_event_count, route_halt);
            orders_->finalize_route(*s, additional_strategy_names_[i], *o, route_halt, pre_net);
            if (route_halt || halt_flag_.load(std::memory_order_acquire))
                return;
        }
    }
}

void engine::apply_l2_delta_batch(const provider::l2_delta_batch& batch)
{
    drain_object_pool_returns();
    if (batch.symbol.empty() || batch.timestamp.time_since_epoch().count() == 0
        || batch.first_update_id == 0 || batch.final_update_id == 0
        || batch.first_update_id > batch.final_update_id
        || batch.updates.empty() || batch.updates.size() > kL2SnapshotMaxLevels)
    {
        trigger_halt("invalid sequenced L2 delta batch");
        return;
    }

    const std::uint16_t sequence_symbol_id =
        orderbook_registry_.symbol_table().id_of(batch.symbol);
    if (sequence_symbol_id == SymbolTable::kInvalidId
        || !l2_sequence_states_[sequence_symbol_id].present)
    {
        trigger_halt("sequenced L2 delta before sequenced snapshot");
        return;
    }
    const auto state = l2_sequence_states_[sequence_symbol_id];
    if (batch.final_update_id <= state.last_update_id)
        return; // complete duplicate/stale frame: deterministic no-op

    const std::uint64_t expected = state.last_update_id + 1;
    const bool first_frame_ok = state.bootstrap_pending
        ? batch.first_update_id <= expected && expected <= batch.final_update_id
        : batch.first_update_id == expected;
    if (!first_frame_ok
        || (batch.has_previous_final_update_id
            && batch.previous_final_update_id != state.last_update_id))
    {
        trigger_halt("sequenced L2 delta gap or out-of-order frame");
        return;
    }

    struct validated_delta { side book_side; Price price; quantity qty; double px; double base_qty; };
    std::array<validated_delta, kL2SnapshotMaxLevels> deltas{};
    for (std::size_t i = 0; i < batch.updates.size(); ++i)
    {
        const auto& update = batch.updates[i];
        if (update.symbol != batch.symbol || update.timestamp != batch.timestamp
            || update.quantity_scale != batch.quantity_scale
            || update.side > 1 || update.price <= 0.0 || update.new_quantity < 0)
        {
            trigger_halt("invalid L2 mutation in sequenced batch");
            return;
        }
        Price book_price;
        std::uint64_t book_qty = 0;
        if (!Price::try_from_double(update.price, book_price)
            || !tt::quantity_scale::rescale_nonnegative(
                update.new_quantity, update.quantity_scale,
                config_.qty_scale, book_qty))
        {
            trigger_halt("invalid L2 mutation quantity or price in sequenced batch");
            return;
        }
        const side book_side = update.side == 0 ? side::buy : side::sell;
        for (std::size_t j = 0; j < i; ++j)
        {
            if (deltas[j].book_side == book_side && deltas[j].price == book_price)
            {
                trigger_halt("duplicate price mutation in sequenced L2 batch");
                return;
            }
        }
        deltas[i] = {book_side, book_price, static_cast<quantity>(book_qty),
                     update.price,
                     tt::quantity_scale::to_base(update.new_quantity,
                                                 update.quantity_scale)};
    }

    // All validation completed before any book/adapter/strategy mutation.
    auto ob = orderbook_registry_.get_or_create(batch.symbol);
    for (std::size_t i = 0; i < batch.updates.size(); ++i)
        ob->apply_l2_update(deltas[i].book_side, deltas[i].price, deltas[i].qty);

    const double best_bid = ob->best_external_bid_price();
    const double best_ask = ob->best_external_ask_price();
    const double l2_mark = (best_bid > 0.0 && best_ask > 0.0)
        ? (best_bid + best_ask) * 0.5 : (best_bid > 0.0 ? best_bid : best_ask);
    if (l2_mark > 0.0)
    {
        last_mid_price_.store(l2_mark, std::memory_order_release);
        last_mark_symbol_ = batch.symbol;
        std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
        last_mark_prices_[batch.symbol] = mark_point{l2_mark, batch.timestamp};
    }
    refresh_top_of_book_atomics(*ob);

    for (std::size_t i = 0; i < batch.updates.size(); ++i)
    {
        if (router_)
            router_->on_l2_update(batch.symbol,
                                  deltas[i].book_side == side::buy
                                      ? order_side::buy : order_side::sell,
                                  deltas[i].px, deltas[i].base_qty,
                                  batch.timestamp);
    }
    l2_sequence_states_[sequence_symbol_id] = {
        batch.final_update_id, false, true};
    last_sim_time_ = batch.timestamp;
    last_decision_ts_ = batch.timestamp + bar_interval_;   // F-08: bar close

    // One post-commit callback is deliberately emitted for the complete frame.
    // The final mutation supplies the legacy event payload; its observation is
    // the fully committed book, never an intermediate side of the batch.
    const auto& last = batch.updates.back();
    const tick_side last_side = last.side == 0 ? tick_side::bid : tick_side::ask;
    auto ev = acquire_pooled(l2_update_pool_, batch.timestamp, batch.symbol,
                             last_side, last.price, last.new_quantity,
                             last.quantity_scale);
    const int64_t recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    ev->set_recv_ns(recv_ns);
    log_event(*ev);
    publish_event(ev);
    if (l2_mark > 0.0) analytics_.on_mark(batch.symbol, l2_mark);

    if (pause_all_.load(std::memory_order_acquire)
        || halt_flag_.load(std::memory_order_acquire)) return;
    std::size_t count = 0;
    bool halted = false;
    if (router_) router_->advance_all(batch.timestamp);
    if (l2_mark > 0.0)
        orders_->drain_due(batch.timestamp, count, halted, batch.symbol);
    if (halted || halt_flag_.load(std::memory_order_acquire)) return;
    if (l2_mark > 0.0 && orders_->evaluate_exits(
            batch.symbol, l2_mark, batch.timestamp, count, recv_ns)) return;

    auto dispatch = [&](std::shared_ptr<IStrategy>& strategy, const std::string& name) {
        if (!strategy || halt_flag_.load(std::memory_order_acquire)) return;
        sync_strategy_account_equity(*strategy);
        if (auto order = strategy->on_l2_update(*ev))
        {
            order->set_recv_ns(recv_ns);
            order->set_strategy_name(name);
            const double pre_net = !name.empty()
                ? portfolio_.get_strategy_position_qty(name, order->get_symbol())
                : (portfolio_.get_positions().count(order->get_symbol()) ? portfolio_.get_positions().at(order->get_symbol()).qty : 0.0);
            bool route_halt = false;
            orders_->route(*order, batch.timestamp, count, route_halt);
            orders_->finalize_route(*strategy, name, *order, route_halt, pre_net);
        }
    };
    dispatch(strategy_, primary_strategy_name_);
    for (std::size_t i = 0; i < additional_strategies_.size(); ++i)
        dispatch(additional_strategies_[i], additional_strategy_names_[i]);
}

void engine::refresh_top_of_book_atomics(const orderbook& ob)
{
    auto* dash = config_.dashboard.get();
    if (!dash) return;
    // bids descending, asks ascending — front() is top of book.
    auto& st = dash->stats();
    const auto infos = ob.get_order_infos();
    const auto& bids = infos.get_bids();
    const auto& asks = infos.get_asks();
    st.best_bid_fp8.store(
        bids.empty() ? std::int64_t{-1}
                     : static_cast<std::int64_t>(
                           std::llround(bids.front().price_.to_double() * 1e8)),
        std::memory_order_relaxed);
    st.best_ask_fp8.store(
        asks.empty() ? std::int64_t{-1}
                     : static_cast<std::int64_t>(
                           std::llround(asks.front().price_.to_double() * 1e8)),
        std::memory_order_relaxed);
}

void engine::dispatch_extras_on_market(const market_event& mkt,
                                       const std::chrono::system_clock::time_point& ts,
                                       std::size_t& event_count)
{
    if (additional_strategies_.empty()) return;
    for (std::size_t i = 0; i < additional_strategies_.size(); ++i)
    {
        if (halt_flag_.load(std::memory_order_acquire)) return;
        auto& s = additional_strategies_[i];
        if (!s) continue;

        sync_strategy_account_equity(*s);
        if (auto o = s->on_market(mkt))
        {
            o->set_recv_ns(mkt.get_recv_ns());
            o->set_strategy_name(additional_strategy_names_[i]);
            const double pre_net = !additional_strategy_names_[i].empty()
                ? portfolio_.get_strategy_position_qty(additional_strategy_names_[i], o->get_symbol())
                : (portfolio_.get_positions().count(o->get_symbol()) ? portfolio_.get_positions().at(o->get_symbol()).qty : 0.0);
            bool halt = false;
            orders_->route(*o, ts, event_count, halt);
            orders_->finalize_route(*s, additional_strategy_names_[i], *o, halt, pre_net);
            if (halt || halt_flag_.load(std::memory_order_acquire)) return;
        }
    }
}

void engine::dispatch_extras_on_tick(const tick_event& te,
                                     const std::chrono::system_clock::time_point& ts,
                                     std::size_t& event_count)
{
    if (additional_strategies_.empty()) return;
    for (std::size_t i = 0; i < additional_strategies_.size(); ++i)
    {
        if (halt_flag_.load(std::memory_order_acquire)) return;
        auto& s = additional_strategies_[i];
        if (!s) continue;

        sync_strategy_account_equity(*s);
        if (auto o = s->on_tick(te))
        {
            o->set_recv_ns(te.get_recv_ns());
            o->set_strategy_name(additional_strategy_names_[i]);
            const double pre_net = !additional_strategy_names_[i].empty()
                ? portfolio_.get_strategy_position_qty(additional_strategy_names_[i], o->get_symbol())
                : (portfolio_.get_positions().count(o->get_symbol()) ? portfolio_.get_positions().at(o->get_symbol()).qty : 0.0);
            bool halt = false;
            orders_->route(*o, ts, event_count, halt);
            orders_->finalize_route(*s, additional_strategy_names_[i], *o, halt, pre_net);
            if (halt || halt_flag_.load(std::memory_order_acquire)) return;
        }
    }
}

// marked_account_equity moved to OrderIntentProcessor::marked_account_equity
// (Phase 1) — public there (unlike this former private engine method)
// precisely because sync_strategy_account_equity below is a second caller
// that stays engine-owned. See order_intent_processor.cpp.

void engine::sync_strategy_account_equity(IStrategy& strategy) const
{
    const double equity = orders_->marked_account_equity(
        last_mark_symbol_, last_mid_price_.load(std::memory_order_relaxed));
    strategy.set_account_equity(std::isfinite(equity) ? equity : 0.0);
}

void engine::process_single_bar(const bar_record& rec, std::size_t& event_count,
                                const std::chrono::system_clock::time_point& timestamp)
{
    drain_object_pool_returns();

    // Apply private-stream cash settlements before any adapter, risk, or
    // strategy action derived from this market record.
    if (!drain_provider_funding_updates())
        return;

    // Already terminal (e.g. risk halt on a prior event / DataBridge race):
    // do not strategy-emit or submit on this bar.
    if (halt_flag_.load(std::memory_order_acquire))
        return;

    // Operator-requested flatten: drain on the next event so the timestamp
    // we close at is from the live stream rather than wall-clock-now.
    if (flatten_request_.exchange(false, std::memory_order_acq_rel))
        orders_->unwind_positions(event_count);

    // Advance adapter clocks first so cancels whose in-flight window has
    // elapsed are drained before this event's matching runs.
    if (router_) router_->advance_all(timestamp);

    market_event mkt(
        timestamp,
        rec.symbol,
        rec.open,
        rec.high,
        rec.low,
        rec.close,
        rec.volume,
        rec.quantity_scale
    );
    const double bar_volume = tt::quantity_scale::to_base(
        mkt.get_volume(), mkt.get_quantity_scale());
    double swept_volume = 0.0;
    mkt.set_recv_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    last_sim_time_ = timestamp;
    last_decision_ts_ = timestamp + bar_interval_;   // F-08: bar close
    last_mid_price_.store(mkt.get_open(), std::memory_order_release);
    last_mark_symbol_ = mkt.get_symbol();
    { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[mkt.get_symbol()] = mark_point{mkt.get_open(), timestamp}; }

    // Drain delayed orders at open mid for each order's symbol (not the
    // event symbol alone — multi-symbol pending must not walk the wrong book).
    bool halt = false;
    orders_->drain_due(timestamp, event_count, halt, mkt.get_symbol());
    // Risk halt (or other terminal) during pending drain: do not continue into
    // strategy / route_order on this bar (was previously loop-scoped only).
    if (halt || halt_flag_.load(std::memory_order_acquire))
        return;

    last_mid_price_.store(mkt.get_close(), std::memory_order_release);
    last_mark_symbol_ = mkt.get_symbol();
    { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[mkt.get_symbol()] = mark_point{mkt.get_close(), timestamp}; }

    {
        // Single stop pass (EL-STREAM-DOUBLE-STOPS): matches batch run().
        // Stops + bar-range sweep before MM/provider fills.
        bool halt = false;
        orders_->check_pending_stops(mkt.get_symbol(), mkt.get_open(),
                            mkt.get_high(), mkt.get_low(),
                            timestamp, event_count, halt);
        swept_volume = orders_->sweep_resting_limits(
            mkt.get_symbol(), mkt.get_low(), mkt.get_high(),
            timestamp, event_count, halt, bar_volume);
        // Match tick/history paths: do not generate new MM/provider fills
        // after a terminal halt on this bar.
        if (halt || halt_flag_.load(std::memory_order_acquire))
            return;
    }

    auto ob = orderbook_registry_.get_or_create(mkt.get_symbol());
    if (!mm_threaded_ &&
        !l2_seeded_symbols_.count(mkt.get_symbol()))
    {
        auto mm_trades = market_maker_.replenish(
            ob, last_mid_price_.load(std::memory_order_relaxed));
        bool halt = false;
        orders_->deliver_mm_book_trades(mkt.get_symbol(), mm_trades,
                               timestamp, event_count, halt);
    }

    // Paper maker-queue: synthetic trade at bar close (lossy, matches shadow bar).
    {
        bool halt_paper = false;
        feed_paper_trade_and_drain(mkt.get_symbol(), mkt.get_close(),
                                   std::max(0.0, bar_volume - swept_volume),
                                   std::nullopt,
                                   timestamp, event_count, halt_paper);
        if (halt_paper || halt_flag_.load(std::memory_order_acquire))
            return;
    }

    if (config_.provider && config_.provider->has_execution())
    {
        config_.provider->on_mid_price(mkt.get_symbol(), last_mid_price_.load(std::memory_order_relaxed));
        auto provider_adapter = config_.provider->get_execution_adapter();

        // Shadow bar-path: feed a synthetic close+volume trade. Lossy
        // (no intra-bar path), but bar-only shadow is low-fidelity anyway.
        if (config_.mode == engine_mode::shadow && provider_adapter)
        {
            provider_adapter->on_trade(mkt.get_symbol(), mkt.get_close(),
                                       bar_volume,
                                       mkt.get_timestamp());
        }

        drain_venue_bracket_meta();
        orders_->drain_async_submit_results(provider_adapter.get());
        if (provider_adapter && config_.mode != engine_mode::shadow)
        {
            bool fill_halt = false;
            if (!fills_->process_adapter_fills(
                    provider_adapter, event_count, fill_halt))
                return;
        }
        else if (provider_adapter)
        {
            bool shadow_halt = false;
            if (!fills_->process_shadow_exchange_fills(
                    provider_adapter, shadow_halt))
                return;
        }
    }

    // Stops already evaluated once above (EL-STREAM-DOUBLE-STOPS).
    // Canonical order continues: exits → strategy → route.

    auto mkt_ptr = acquire_pooled(market_pool_,mkt);
    log_event(mkt);
    publish_event(mkt_ptr);
    if (!config_.has_async_analytics())
        analytics_.on_event(mkt_ptr);
    else
        analytics_.on_mark(mkt.get_symbol(), mkt.get_close());
    event_count++;

    if (orders_->evaluate_exits(mkt.get_symbol(),
                       mkt.get_open(), mkt.get_low(), mkt.get_high(), mkt.get_close(),
                       timestamp, event_count, mkt.get_recv_ns()))
        return;

    if (!strategy_ || halt_flag_.load(std::memory_order_acquire))
        return;

    sync_strategy_account_equity(*strategy_);
    auto order_opt = strategy_->on_market(mkt);
    if (order_opt && !halt_flag_.load(std::memory_order_acquire))
    {
        order_opt->set_recv_ns(mkt.get_recv_ns());
        if (!primary_strategy_name_.empty())
            order_opt->set_strategy_name(primary_strategy_name_);
        const double pre_net = !primary_strategy_name_.empty()
            ? portfolio_.get_strategy_position_qty(primary_strategy_name_, order_opt->get_symbol())
            : (portfolio_.get_positions().count(order_opt->get_symbol()) ? portfolio_.get_positions().at(order_opt->get_symbol()).qty : 0.0);
        bool route_halt = false;
        orders_->route(*order_opt, timestamp, event_count, route_halt);
        orders_->finalize_route(*strategy_, primary_strategy_name_, *order_opt, route_halt, pre_net);
    }
    if (!halt_flag_.load(std::memory_order_acquire))
        dispatch_extras_on_market(mkt, timestamp, event_count);
}

void engine::process_single_tick(const tick_record& rec, std::size_t& event_count)
{
    drain_object_pool_returns();

    if (!drain_provider_funding_updates())
        return;

    if (halt_flag_.load(std::memory_order_acquire))
        return;

    if (flatten_request_.exchange(false, std::memory_order_acq_rel))
        orders_->unwind_positions(event_count);

    if (router_) router_->advance_all(rec.timestamp);

    tick_side ts = tick_side::unknown;
    if (rec.side == data_tick_side::bid) ts = tick_side::bid;
    else if (rec.side == data_tick_side::ask) ts = tick_side::ask;

    tick_event te(rec.timestamp, rec.symbol, rec.price, rec.quantity, ts,
                  rec.quantity_scale);
    const double tick_qty = tt::quantity_scale::to_base(
        rec.quantity, rec.quantity_scale);
    te.set_recv_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    last_sim_time_ = rec.timestamp;
    last_decision_ts_ = rec.timestamp;   // F-08: point observation
    last_mid_price_.store(rec.price, std::memory_order_release);
    last_mark_symbol_ = rec.symbol;
    { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[rec.symbol] = mark_point{rec.price, rec.timestamp}; }

    {
        DEBUG_STAGE(stage_timer_, mm_replenish);
        auto ob = orderbook_registry_.get_or_create(rec.symbol);
        if (!l2_seeded_symbols_.count(rec.symbol))
        {
            auto mm_trades = market_maker_.replenish(
                ob, last_mid_price_.load(std::memory_order_relaxed));
            bool halt = false;
            orders_->deliver_mm_book_trades(rec.symbol, mm_trades,
                                   rec.timestamp, event_count, halt);
        }
    }

    // Paper maker-queue: real tick print advances QueueAware size_ahead.
    {
        bool halt_paper = false;
        feed_paper_trade_and_drain(rec.symbol, rec.price,
                                   tick_qty,
                                   rec.side == data_tick_side::bid
                                       ? std::optional<order_side>{order_side::buy}
                                       : rec.side == data_tick_side::ask
                                           ? std::optional<order_side>{order_side::sell}
                                           : std::nullopt,
                                   rec.timestamp, event_count, halt_paper);
        if (halt_paper || halt_flag_.load(std::memory_order_acquire))
            return;
    }

    if (config_.provider && config_.provider->has_execution())
    {
        config_.provider->on_mid_price(rec.symbol, last_mid_price_.load(std::memory_order_relaxed));
        auto provider_adapter = config_.provider->get_execution_adapter();

        // Must fire BEFORE poll_fills so fills this tick are drained here.
        if (config_.mode == engine_mode::shadow && provider_adapter)
        {
            provider_adapter->on_trade(rec.symbol, rec.price,
                                       tick_qty,
                                       rec.side == data_tick_side::bid
                                           ? std::optional<order_side>{order_side::buy}
                                           : rec.side == data_tick_side::ask
                                               ? std::optional<order_side>{order_side::sell}
                                               : std::nullopt,
                                       rec.timestamp);
        }

        drain_venue_bracket_meta();
        orders_->drain_async_submit_results(provider_adapter.get());
        if (provider_adapter && config_.mode != engine_mode::shadow)
        {
            bool fill_halt = false;
            if (!fills_->process_adapter_fills(
                    provider_adapter, event_count, fill_halt))
                return;
        }
        else if (provider_adapter)
        {
            bool shadow_halt = false;
            if (!fills_->process_shadow_exchange_fills(
                    provider_adapter, shadow_halt))
                return;
        }
    }

    bool halt = false;

    {
        DEBUG_STAGE(stage_timer_, pending_drain);
        orders_->drain_due(rec.timestamp, event_count, halt, rec.symbol);
    }
    if (halt || halt_flag_.load(std::memory_order_acquire)) return;

    {
        DEBUG_STAGE(stage_timer_, stop_check);
        orders_->check_pending_stops(rec.symbol, rec.price, rec.price, rec.price,
                            rec.timestamp, event_count, halt);
    }
    if (halt || halt_flag_.load(std::memory_order_acquire)) return;

    auto tick_ptr = acquire_pooled(tick_pool_,te);
    log_event(te);
    {
        DEBUG_STAGE(stage_timer_, ring_publish);
        publish_event(tick_ptr);
    }
    if (!config_.has_async_analytics())
        analytics_.on_event(tick_ptr);
    else
        analytics_.on_mark(rec.symbol, rec.price);
    event_count++;

    if (orders_->evaluate_exits(rec.symbol, rec.price, rec.timestamp,
                       event_count, te.get_recv_ns()))
        return;

    if (!strategy_ || halt_flag_.load(std::memory_order_acquire))
        return;

    std::optional<order_event> order_opt;
    {
        DEBUG_STAGE(stage_timer_, strategy);
        sync_strategy_account_equity(*strategy_);
        order_opt = strategy_->on_tick(te);
    }
    if (order_opt && !halt_flag_.load(std::memory_order_acquire))
    {
        order_opt->set_recv_ns(te.get_recv_ns());
        if (!primary_strategy_name_.empty())
            order_opt->set_strategy_name(primary_strategy_name_);
        const double pre_net = !primary_strategy_name_.empty()
            ? portfolio_.get_strategy_position_qty(primary_strategy_name_, order_opt->get_symbol())
            : (portfolio_.get_positions().count(order_opt->get_symbol()) ? portfolio_.get_positions().at(order_opt->get_symbol()).qty : 0.0);
        orders_->route(*order_opt, rec.timestamp, event_count, halt);
        orders_->finalize_route(*strategy_, primary_strategy_name_, *order_opt, halt, pre_net);
    }
    if (!halt_flag_.load(std::memory_order_acquire))
        dispatch_extras_on_tick(te, rec.timestamp, event_count);
}

StreamResult engine::run_streaming(std::shared_ptr<DataBridge<bar_record>> bridge)
{
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    prepare_event_logging();
    clear_pending_state();

#ifdef HAS_QUESTDB
    questdb_begin();
#endif
    prepare_mark_prices_for_run(/*symbol_hint=*/16);
    start_workers();
    pin_event_loop_thread();

    bridge->set_halt_flag(&halt_flag_);

    const auto start = std::chrono::high_resolution_clock::now();
    std::size_t event_count = 0;
    std::size_t bar_index = 0;

    auto* dash = config_.dashboard.get();
    bool needs_periodic_tick = !dash;
    if (dash)
    {
        dash->set_state(truetest::ui::connection_state::waiting);
        dash->push_event(truetest::ui::event_severity::notice,
                         "streaming: waiting for first bar");
    }
    else
    {
        std::cout << "\rStreaming: waiting for data..." << std::flush;
    }

    auto last_report_time = std::chrono::steady_clock::now();
    std::chrono::system_clock::time_point last_good_bar_ts{};

    typename DataBridge<bar_record>::idle_callback funding_idle;
    if (provider_funding_ingress_)
        funding_idle = [this] { (void)drain_provider_funding_updates(); };
    auto stream_result = bridge->run_streaming(data_handler_, [&](const bar_record& rec) {
        auto timestamp = tt::date_parse::resolve_bar_clock(
            rec.open_time_ms, rec.date, last_good_bar_ts);
        last_good_bar_ts = timestamp;
        process_single_bar(rec, event_count, timestamp);
        bar_index++;

        if (dash)
        {
            auto& st = dash->stats();
            if (bar_index == 1)
                dash->set_state(truetest::ui::connection_state::live);
            st.events_total.store(bar_index, std::memory_order_relaxed);
            st.fills_total.store(portfolio_.get_total_fills(),
                                 std::memory_order_relaxed);
            st.trades_total.store(portfolio_.get_total_trades(),
                                  std::memory_order_relaxed);
            st.last_price_fp8.store(
                static_cast<std::int64_t>(rec.close * 1e8),
                std::memory_order_relaxed);
            st.realized_pnl_fp4.store(
                static_cast<std::int64_t>(std::llround(analytics_.realized_pnl() * 1e4)),
                std::memory_order_relaxed);
            {
                const auto& positions = portfolio_.get_positions();
                double qty = 0.0, cost = 0.0;
                if (auto it = positions.find(rec.symbol); it != positions.end())
                {
                    qty  = it->second.qty;
                    cost = it->second.cost_basis;
                }
                const double unreal = qty * rec.close - cost;
                st.unrealized_pnl_fp4.store(
                    static_cast<std::int64_t>(std::llround(unreal * 1e4)),
                    std::memory_order_relaxed);
                st.position_qty_fp8.store(
                    static_cast<std::int64_t>(std::llround(qty * 1e8)),
                    std::memory_order_relaxed);
            }
            // Sign-flip so "%+.2f%%" renders "-15.50%" not "+15.50%".
            st.drawdown_fp4.store(
                -static_cast<std::int64_t>(std::llround(analytics_.max_drawdown_pct() * 1e2)),
                std::memory_order_relaxed);
            st.win_rate_bps.store(
                static_cast<std::uint32_t>(std::lround(analytics_.win_rate_pct() * 100.0)),
                std::memory_order_relaxed);
            // Simulated time so horizon compares correctly in replay.
            adverse_selection_.on_mark(rec.symbol, rec.close, timestamp);
            st.toxicity_bps_fp2.store(
                static_cast<std::int32_t>(std::lround(
                    adverse_selection_.mean_bps() * 100.0)),
                std::memory_order_relaxed);
            st.toxicity_samples.store(
                static_cast<std::uint32_t>(adverse_selection_.sample_count()),
                std::memory_order_relaxed);
            write_adapter_diagnostics(st);
        }

        {
            std::lock_guard<std::mutex> lk(switch_mu_);
            if (!pending_symbol_.empty()) {
                std::string new_sym = std::move(pending_symbol_);
                pending_symbol_.clear();
                switch_symbol(new_sym);
            }
            if (!pending_strategy_.empty()) {
                std::string new_strat = std::move(pending_strategy_);
                pending_strategy_.clear();
                strategy_params params;
                params.balance = config_.initial_balance;
                // F-10: StrategyFactory now resolves through StrategyRegistry
                // and throws on an unknown name instead of silently handing
                // back mean-reversion. An operator typo must not swap the
                // running strategy, and must not abort a live stream either.
                if (!StrategyFactory::has(new_strat)) {
                    std::cerr << "  ! unknown --strategy '" << new_strat
                              << "'; keeping the running strategy\n";
                } else {
                    set_strategy(StrategyFactory::create(new_strat, params));
                }
            }
        }

        if (needs_periodic_tick)
        {
            auto now_report = std::chrono::steady_clock::now();
            if (now_report - last_report_time >= std::chrono::milliseconds(200))
            {
                if (!dash)
                    std::cout << "\rStreaming: " << bar_index
                              << " bars | Fills: " << portfolio_.get_total_fills()
                              << " | Round-trips: " << portfolio_.get_total_trades()
                              << std::flush;
                maybe_questdb_tick();
                last_report_time = now_report;
            }
        }
    }, funding_idle);

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Force-drain is a research/batch EOS convention. Operator stop and every
    // failure path must return without creating any new venue mutation; live
    // session shutdown quiesces and kills through LiveSafetySession instead.
    if (stream_result.termination == stream_termination::clean_eof
        && config_.mode == engine_mode::backtest)
    {
        bool halt = halt_flag_.load(std::memory_order_acquire);
        orders_->finalize_end_of_stream(event_count, halt);
    }

    stop_workers();
    finalize_inline_event_log();
#ifdef HAS_QUESTDB
    questdb_end();
#endif
    if (!stream_result.success())
        run_failed_.store(true, std::memory_order_release);
    if (stream_result.success() && !run_succeeded())
        stream_result.termination = stream_termination::runtime_failure;
    const bool overall_ok = stream_result.success() && run_succeeded();
    if (dash)
        dash->set_state(overall_ok ? truetest::ui::connection_state::closed
                                   : truetest::ui::connection_state::halted);
    else
        std::cout << std::endl
                  << (overall_ok ? "Streaming complete: " : "Streaming failed: ")
                  << bar_index << " bars, " << portfolio_.get_total_trades()
                  << " trades in " << (elapsed_ms > 0 ? elapsed_ms : 1)
                  << " ms" << std::endl;
    return stream_result;
}

StreamResult engine::run_streaming(std::shared_ptr<DataBridge<tick_record>> bridge)
{
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    prepare_event_logging();
    clear_pending_state();

#ifdef HAS_QUESTDB
    questdb_begin();
#endif
    prepare_mark_prices_for_run(/*symbol_hint=*/16);
    start_workers();
    pin_event_loop_thread();

    bridge->set_halt_flag(&halt_flag_);

    const auto start = std::chrono::high_resolution_clock::now();
    std::size_t event_count = 0;
    std::size_t tick_count = 0;

    auto* dash = config_.dashboard.get();
    bool needs_periodic_tick = !dash;
    if (dash)
    {
        dash->set_state(truetest::ui::connection_state::waiting);
        dash->push_event(truetest::ui::event_severity::notice,
                         "streaming: waiting for first tick");
    }
    else
    {
        std::cout << "\rStreaming: waiting for data..." << std::flush;
    }

    auto last_report_time = std::chrono::steady_clock::now();

    typename DataBridge<tick_record>::idle_callback funding_idle;
    if (provider_funding_ingress_)
        funding_idle = [this] { (void)drain_provider_funding_updates(); };
    auto stream_result = bridge->run_streaming(data_handler_, [&](const tick_record& rec) {
        process_single_tick(rec, event_count);
        tick_count++;

        if (dash)
        {
            auto& st = dash->stats();
            if (tick_count == 1)
                dash->set_state(truetest::ui::connection_state::live);
            st.events_total.store(tick_count, std::memory_order_relaxed);
            st.fills_total.store(portfolio_.get_total_fills(),
                                 std::memory_order_relaxed);
            st.trades_total.store(portfolio_.get_total_trades(),
                                  std::memory_order_relaxed);
            st.last_price_fp8.store(
                static_cast<std::int64_t>(rec.price * 1e8),
                std::memory_order_relaxed);
            st.realized_pnl_fp4.store(
                static_cast<std::int64_t>(std::llround(analytics_.realized_pnl() * 1e4)),
                std::memory_order_relaxed);
            {
                const auto& positions = portfolio_.get_positions();
                double qty = 0.0, cost = 0.0;
                if (auto it = positions.find(rec.symbol); it != positions.end())
                {
                    qty  = it->second.qty;
                    cost = it->second.cost_basis;
                }
                const double unreal = qty * rec.price - cost;
                st.unrealized_pnl_fp4.store(
                    static_cast<std::int64_t>(std::llround(unreal * 1e4)),
                    std::memory_order_relaxed);
                st.position_qty_fp8.store(
                    static_cast<std::int64_t>(std::llround(qty * 1e8)),
                    std::memory_order_relaxed);
            }
            st.drawdown_fp4.store(
                -static_cast<std::int64_t>(std::llround(analytics_.max_drawdown_pct() * 1e2)),
                std::memory_order_relaxed);
            st.win_rate_bps.store(
                static_cast<std::uint32_t>(std::lround(analytics_.win_rate_pct() * 100.0)),
                std::memory_order_relaxed);
            adverse_selection_.on_mark(rec.symbol, rec.price, rec.timestamp);
            st.toxicity_bps_fp2.store(
                static_cast<std::int32_t>(std::lround(
                    adverse_selection_.mean_bps() * 100.0)),
                std::memory_order_relaxed);
            st.toxicity_samples.store(
                static_cast<std::uint32_t>(adverse_selection_.sample_count()),
                std::memory_order_relaxed);
            write_adapter_diagnostics(st);
        }

        {
            std::lock_guard<std::mutex> lk(switch_mu_);
            if (!pending_symbol_.empty()) {
                std::string new_sym = std::move(pending_symbol_);
                pending_symbol_.clear();
                switch_symbol(new_sym);
            }
            if (!pending_strategy_.empty()) {
                std::string new_strat = std::move(pending_strategy_);
                pending_strategy_.clear();
                strategy_params params;
                params.balance = config_.initial_balance;
                // F-10: StrategyFactory now resolves through StrategyRegistry
                // and throws on an unknown name instead of silently handing
                // back mean-reversion. An operator typo must not swap the
                // running strategy, and must not abort a live stream either.
                if (!StrategyFactory::has(new_strat)) {
                    std::cerr << "  ! unknown --strategy '" << new_strat
                              << "'; keeping the running strategy\n";
                } else {
                    set_strategy(StrategyFactory::create(new_strat, params));
                }
            }
        }

        if (needs_periodic_tick)
        {
            auto now_report = std::chrono::steady_clock::now();
            if (now_report - last_report_time >= std::chrono::milliseconds(200))
            {
                if (!dash)
                    std::cout << "\rStreaming: " << tick_count
                              << " ticks | Fills: " << portfolio_.get_total_fills()
                              << " | Round-trips: " << portfolio_.get_total_trades()
                              << std::flush;
                maybe_questdb_tick();
                last_report_time = now_report;
            }
        }
    }, funding_idle);

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    if (stream_result.termination == stream_termination::clean_eof
        && config_.mode == engine_mode::backtest)
    {
        bool halt = halt_flag_.load(std::memory_order_acquire);
        orders_->finalize_end_of_stream(event_count, halt);
    }

    stop_workers();
    finalize_inline_event_log();
#ifdef HAS_QUESTDB
    questdb_end();
#endif
    if (!stream_result.success())
        run_failed_.store(true, std::memory_order_release);
    if (stream_result.success() && !run_succeeded())
        stream_result.termination = stream_termination::runtime_failure;
    const bool overall_ok = stream_result.success() && run_succeeded();
    if (dash)
        dash->set_state(overall_ok ? truetest::ui::connection_state::closed
                                   : truetest::ui::connection_state::halted);
    else
        std::cout << std::endl
                  << (overall_ok ? "Streaming complete: " : "Streaming failed: ")
                  << tick_count << " ticks, " << portfolio_.get_total_trades()
                  << " trades in " << (elapsed_ms > 0 ? elapsed_ms : 1)
                  << " ms" << std::endl;
    return stream_result;
}

StreamResult engine::run_streaming(std::shared_ptr<DataBridge<provider::event>> bridge)
{
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    prepare_event_logging();
    clear_pending_state();

#ifdef HAS_QUESTDB
    questdb_begin();
#endif
    prepare_mark_prices_for_run(/*symbol_hint=*/16);
    start_workers();
    pin_event_loop_thread();

    bridge->set_halt_flag(&halt_flag_);

    const auto start = std::chrono::high_resolution_clock::now();
    std::size_t event_count = 0;
    std::size_t record_count = 0;

    auto* dash = config_.dashboard.get();
    bool needs_periodic_tick = !dash;
    if (dash)
    {
        dash->set_state(truetest::ui::connection_state::waiting);
        dash->push_event(truetest::ui::event_severity::notice,
                         "streaming: waiting for first event");
    }
    else
    {
        std::cout << "\rStreaming: waiting for data..." << std::flush;
    }

    auto last_report_time = std::chrono::steady_clock::now();

    // current_event_ts tracks sim time of the last price-bearing event —
    // the clock used for AdverseSelectionTracker::on_mark so horizons stay
    // consistent in historical replay. L2 + status don't advance it.
    std::chrono::system_clock::time_point current_event_ts =
        std::chrono::system_clock::now();

    typename DataBridge<provider::event>::idle_callback funding_idle;
    if (provider_funding_ingress_)
        funding_idle = [this] { (void)drain_provider_funding_updates(); };
    auto stream_result = bridge->run_streaming(data_handler_, [&](const provider::event& ev) {
        std::visit([&](const auto& e) {
            using E = std::decay_t<decltype(e)>;

            if constexpr (std::is_same_v<E, provider::bar>)
            {
                auto rec = provider::to_bar_record(e);
                auto timestamp = tt::date_parse::resolve_bar_clock(
                    rec.open_time_ms, rec.date, current_event_ts);
                current_event_ts = timestamp;
                process_single_bar(rec, event_count, timestamp);
                record_count++;
            }
            else if constexpr (std::is_same_v<E, provider::tick>)
            {
                auto rec = provider::to_tick_record(e);
                current_event_ts = rec.timestamp;
                process_single_tick(rec, event_count);
                record_count++;
            }
            else if constexpr (std::is_same_v<E, provider::l2_snapshot>)
            {
                if (!drain_provider_funding_updates()) return;
                std::vector<l2_level> bids;
                bids.reserve(e.bids.size());
                for (const auto& lvl : e.bids)
                    bids.push_back({lvl.price, lvl.quantity});
                std::vector<l2_level> asks;
                asks.reserve(e.asks.size());
                for (const auto& lvl : e.asks)
                    asks.push_back({lvl.price, lvl.quantity});
                apply_l2_snapshot(e.symbol, bids, asks, e.timestamp,
                                  e.quantity_scale, e.last_update_id);
                // (forward to queue models now centralized inside apply_l2_snapshot)
            }
            else if constexpr (std::is_same_v<E, provider::l2_update>)
            {
                if (!drain_provider_funding_updates()) return;
                tick_side ts = tick_side::unknown;
                if (e.side == 0) ts = tick_side::bid;
                else if (e.side == 1) ts = tick_side::ask;
                apply_l2_update(e.symbol, ts, e.price, e.new_quantity,
                                e.timestamp, e.quantity_scale);
                // (forward to queue models now centralized inside apply_l2_update)
            }
            else if constexpr (std::is_same_v<E, provider::l2_delta_batch>)
            {
                if (!drain_provider_funding_updates()) return;
                apply_l2_delta_batch(e);
            }
        }, ev);

        if (dash)
        {
            auto& st = dash->stats();
            if (record_count == 1)
                dash->set_state(truetest::ui::connection_state::live);
            st.events_total.store(record_count, std::memory_order_relaxed);
            st.fills_total.store(portfolio_.get_total_fills(),
                                 std::memory_order_relaxed);
            st.trades_total.store(portfolio_.get_total_trades(),
                                  std::memory_order_relaxed);
            if (last_mid_price_.load(std::memory_order_relaxed) > 0.0)
                st.last_price_fp8.store(
                    static_cast<std::int64_t>(last_mid_price_.load(std::memory_order_relaxed) * 1e8),
                    std::memory_order_relaxed);
            st.realized_pnl_fp4.store(
                static_cast<std::int64_t>(std::llround(analytics_.realized_pnl() * 1e4)),
                std::memory_order_relaxed);
            // Only mark once we've seen a price — L2/status may arrive first.
            if (last_mid_price_.load(std::memory_order_relaxed) > 0.0 && !last_mark_symbol_.empty())
            {
                const auto& positions = portfolio_.get_positions();
                double qty = 0.0, cost = 0.0;
                if (auto it = positions.find(last_mark_symbol_); it != positions.end())
                {
                    qty  = it->second.qty;
                    cost = it->second.cost_basis;
                }
                const double unreal = qty * last_mid_price_.load(std::memory_order_relaxed) - cost;
                st.unrealized_pnl_fp4.store(
                    static_cast<std::int64_t>(std::llround(unreal * 1e4)),
                    std::memory_order_relaxed);
                st.position_qty_fp8.store(
                    static_cast<std::int64_t>(std::llround(qty * 1e8)),
                    std::memory_order_relaxed);
            }
            st.drawdown_fp4.store(
                -static_cast<std::int64_t>(std::llround(analytics_.max_drawdown_pct() * 1e2)),
                std::memory_order_relaxed);
            st.win_rate_bps.store(
                static_cast<std::uint32_t>(std::lround(analytics_.win_rate_pct() * 100.0)),
                std::memory_order_relaxed);
            if (last_mid_price_.load(std::memory_order_relaxed) > 0.0 && !last_mark_symbol_.empty())
            {
                adverse_selection_.on_mark(last_mark_symbol_,
                                           last_mid_price_.load(std::memory_order_relaxed),
                                           current_event_ts);
            }
            st.toxicity_bps_fp2.store(
                static_cast<std::int32_t>(std::lround(
                    adverse_selection_.mean_bps() * 100.0)),
                std::memory_order_relaxed);
            st.toxicity_samples.store(
                static_cast<std::uint32_t>(adverse_selection_.sample_count()),
                std::memory_order_relaxed);
            write_adapter_diagnostics(st);
        }

        if (needs_periodic_tick)
        {
            auto now_report = std::chrono::steady_clock::now();
            if (now_report - last_report_time >= std::chrono::milliseconds(200))
            {
                if (!dash)
                    std::cout << "\rStreaming: " << record_count
                              << " events | Fills: " << portfolio_.get_total_fills()
                              << " | Round-trips: " << portfolio_.get_total_trades()
                              << std::flush;
                maybe_questdb_tick();
                last_report_time = now_report;
            }
        }
    }, funding_idle);

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    if (stream_result.termination == stream_termination::clean_eof
        && config_.mode == engine_mode::backtest)
    {
        bool halt = halt_flag_.load(std::memory_order_acquire);
        orders_->finalize_end_of_stream(event_count, halt);
    }

    stop_workers();
    finalize_inline_event_log();
#ifdef HAS_QUESTDB
    questdb_end();
#endif
    if (!stream_result.success())
        run_failed_.store(true, std::memory_order_release);
    if (stream_result.success() && !run_succeeded())
        stream_result.termination = stream_termination::runtime_failure;
    const bool overall_ok = stream_result.success() && run_succeeded();
    if (dash)
        dash->set_state(overall_ok ? truetest::ui::connection_state::closed
                                   : truetest::ui::connection_state::halted);
    else
        std::cout << std::endl
                  << (overall_ok ? "Streaming complete: " : "Streaming failed: ")
                  << record_count << " events, " << portfolio_.get_total_trades()
                  << " trades in " << (elapsed_ms > 0 ? elapsed_ms : 1)
                  << " ms" << std::endl;
    return stream_result;
}

void engine::run_tick_data()
{
    prepare_event_logging();

    clear_pending_state();

#ifdef HAS_DEBUG
    memory_sampler_.set_start(debug::memory_snapshot::capture());
#endif

    setup_event_loop_infra();

    // docs/internal/data-pipeline.md#D-02: tick path uses tick_at / tick_count (no public vector).
    const auto n = data_handler_->tick_count();
    prepare_mark_prices_for_run(/*symbol_hint=*/16);
    const auto start = std::chrono::high_resolution_clock::now();

    if (config_.show_progress) {
        std::cout << "\rProgress: 0.000% | Trades executed: 0" << std::flush;
    }

    std::size_t event_count = 0;
    bool halt_requested = false;
    auto last_report_time = std::chrono::steady_clock::now();

    // Tick path: aggregate bars for analytics/marks only. Strategy market
    // handling is on_tick exclusively — dual on_tick + on_market double-fired
    // indicators/entries on strategies that implement both (EL-01).
    // Exit evaluation is per-tick only (EL-TICK-BAR-EXIT-LA): synthetic 1s
    // bars emit the completed prior interval after a later tick arrives, so
    // OHLC on_bar would test adverse extremes printed before entry arm and
    // look-ahead stop-out vs pure on_price.
    BarAggregator bar_agg(std::chrono::seconds(1), [&](const market_event& bar)
    {
        int64_t bar_recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        auto bar_ptr = acquire_pooled(market_pool_,bar);
        bar_ptr->set_recv_ns(bar_recv_ns);
        publish_event(bar_ptr);
        if (!config_.has_async_analytics())
            analytics_.on_event(bar_ptr);
        else
            analytics_.on_mark(bar.get_symbol(), bar.get_close());
        (void)halt_requested;
    });

    for (std::size_t i = 0; i < n && !halt_requested
             && !halt_flag_.load(std::memory_order_acquire)
             && !worker_failed_.load(std::memory_order_acquire); ++i)
    {
        const auto& tick = data_handler_->tick_at(i);

        last_sim_time_ = tick.timestamp;
        last_decision_ts_ = tick.timestamp;   // F-08: point observation
        last_mid_price_.store(tick.price, std::memory_order_release);
        last_mark_symbol_ = tick.symbol;
        { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[tick.symbol] = mark_point{tick.price, tick.timestamp}; }

        // Latency-gated cancel windows need clock advance on the tick path too.
        if (router_) router_->advance_all(tick.timestamp);

        {
            DEBUG_STAGE(stage_timer_, mm_replenish);
            auto ob = orderbook_registry_.get_or_create(tick.symbol);
            if (!l2_seeded_symbols_.count(tick.symbol))
            {
                auto mm_trades = market_maker_.replenish(
                    ob, last_mid_price_.load(std::memory_order_relaxed));
                orders_->deliver_mm_book_trades(tick.symbol, mm_trades, tick.timestamp,
                                       event_count, halt_requested);
            }
        }

        {
            DEBUG_STAGE(stage_timer_, pending_drain);
            orders_->drain_due(tick.timestamp, event_count, halt_requested,
                                 tick.symbol);
        }
        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

        {
            DEBUG_STAGE(stage_timer_, stop_check);
            orders_->check_pending_stops(tick.symbol, tick.price, tick.price,
                                tick.price, tick.timestamp, event_count,
                                halt_requested);
        }
        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

        feed_paper_trade_and_drain(tick.symbol, tick.price,
                                   tt::quantity_scale::to_base(
                                       tick.quantity, tick.quantity_scale),
                                   tick.side == data_tick_side::bid
                                       ? std::optional<order_side>{order_side::buy}
                                       : tick.side == data_tick_side::ask
                                           ? std::optional<order_side>{order_side::sell}
                                           : std::nullopt,
                                   tick.timestamp, event_count, halt_requested);
        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

        tick_side ts = tick_side::unknown;
        if (tick.side == data_tick_side::bid) ts = tick_side::bid;
        else if (tick.side == data_tick_side::ask) ts = tick_side::ask;

        tick_event te(tick.timestamp, tick.symbol, tick.price, tick.quantity, ts,
                      tick.quantity_scale);
        te.set_recv_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        auto tick_ptr = acquire_pooled(tick_pool_,te);
        log_event(te);
        {
            DEBUG_STAGE(stage_timer_, ring_publish);
            publish_event(tick_ptr);
        }
        if (!config_.has_async_analytics())
            analytics_.on_event(tick_ptr);
        else
            analytics_.on_mark(tick.symbol, tick.price);
        event_count++;

        if (orders_->evaluate_exits(tick.symbol, tick.price, tick.timestamp,
                           event_count, te.get_recv_ns()))
            break;

        // EL-TICK-NULL-STRATEGY: match process_single_tick / run() null guard.
        if (!strategy_ || halt_flag_.load(std::memory_order_acquire))
        {
            if (!bar_agg.on_tick(tick.symbol, tick.price, tick.quantity,
                                 tick.timestamp, tick.quantity_scale))
            {
                trigger_halt("tick bar aggregator symbol capacity or scale invalid");
                halt_requested = true;
                break;
            }
            continue;
        }

        std::optional<order_event> order_opt;
        {
            DEBUG_STAGE(stage_timer_, strategy);
            sync_strategy_account_equity(*strategy_);
            order_opt = strategy_->on_tick(te);
        }
        if (order_opt)
        {
            order_opt->set_recv_ns(te.get_recv_ns());
            if (!primary_strategy_name_.empty())
                order_opt->set_strategy_name(primary_strategy_name_);
            const double pre_net = !primary_strategy_name_.empty()
                ? portfolio_.get_strategy_position_qty(primary_strategy_name_, order_opt->get_symbol())
                : (portfolio_.get_positions().count(order_opt->get_symbol()) ? portfolio_.get_positions().at(order_opt->get_symbol()).qty : 0.0);
            orders_->route(*order_opt, tick.timestamp, event_count, halt_requested);
            if (strategy_)
                orders_->finalize_route(*strategy_, primary_strategy_name_, *order_opt,
                                        halt_requested, pre_net);
        }
        if (!halt_flag_.load(std::memory_order_acquire))
            dispatch_extras_on_tick(te, tick.timestamp, event_count);
        if (halt_requested) break;

        if (!bar_agg.on_tick(tick.symbol, tick.price, tick.quantity,
                             tick.timestamp, tick.quantity_scale))
        {
            trigger_halt("tick bar aggregator symbol capacity or scale invalid");
            halt_requested = true;
            break;
        }

        {
            auto now_report = std::chrono::steady_clock::now();
            if ((i + 1) == n || now_report - last_report_time >= std::chrono::milliseconds(200))
            {
                const double progress = ((i + 1) * 100.0) / static_cast<double>(n);
                if (config_.show_progress) {
                    std::cout << "\rProgress: " << std::fixed << std::setprecision(3) << progress
                              << "% | Trades executed: " << portfolio_.get_total_trades()
                              << std::flush;
                }
                last_report_time = now_report;
                maybe_questdb_tick();
            }
        }
    }

    bar_agg.flush();

    orders_->finalize_end_of_stream(event_count, halt_requested);

    report_run_summary(event_count, start);

    teardown_event_loop_infra();

#ifdef HAS_DEBUG
    memory_sampler_.set_end(debug::memory_snapshot::capture());
    {
        debug::DebugReport report;
        std::vector<std::pair<const char*, const debug::thread_utilization*>> worker_utils;
        if (logging_worker_)    worker_utils.push_back({"logging", &logging_worker_->debug_utilization()});
        if (risk_worker_)       worker_utils.push_back({"risk", &risk_worker_->debug_utilization()});
        if (stats_worker_)      worker_utils.push_back({"stats", &stats_worker_->debug_utilization()});
        if (observer_worker_)   worker_utils.push_back({"observer", &observer_worker_->debug_utilization()});
        if (risk_stats_worker_) worker_utils.push_back({"risk_stats", &risk_stats_worker_->debug_utilization()});
        if (mm_worker_)         worker_utils.push_back({"market_maker", &mm_worker_->debug_utilization()});

        std::vector<const debug::ring_diagnostics*> ring_diags = {
            &logging_diag_, &risk_diag_, &stats_diag_,
            &observer_diag_, &risk_stats_diag_, &mm_diag_
        };

        report.log_all(stage_timer_, memory_sampler_, worker_utils, ring_diags);
    }
#endif
}

void engine::run_replay(const std::string& log_path,
                        int64_t replay_from_us,
                        int64_t replay_to_us)
{
    if (authoritative_replay_started_)
        throw std::runtime_error(
            "authoritative ledger replay is one-shot per engine instance");
    authoritative_replay_started_ = true;
    if (config_.is_threaded())
        throw std::runtime_error(
            "ledger replay requires --thread-preset inline");
    if (replay_from_us != 0)
        throw std::runtime_error(
            "partial ledger replay requires prefix state; --replay-from is refused");
    if (replay_to_us != INT64_MAX)
        throw std::runtime_error(
            "bounded ledger replay requires append-sequence state; --replay-to is refused");
    if (!config_.event_log_path.empty())
        throw std::runtime_error(
            "ledger replay cannot also write --log-events; choose a separate audit workflow");
    const auto before_report = analytics_.generate_report();
    if (order_tracker_.orders_seen() != 0
        || !portfolio_.get_positions().empty()
        || !portfolio_.get_lots().empty()
        || portfolio_.get_cash() != config_.initial_balance
        || before_report.total_orders != 0
        || before_report.total_fills != 0
        || analytics_.total_funding_pnl() != 0.0)
        throw std::runtime_error(
            "authoritative ledger replay requires a pristine engine state");

    EventReplayer preflight(log_path, /*from_us=*/0, replay_to_us);
    if (preflight.file_version() != EVENT_LOG_FILE_VERSION)
        throw std::runtime_error(
            "authoritative ledger replay requires a current v3 event log; "
            "legacy logs remain inspection-only");
    if (!preflight.file_finalized())
        throw std::runtime_error(
            "authoritative ledger replay requires a finalized event log; "
            "an in-progress or crash-truncated prefix is inspection-only");
    if (preflight.file_segmented())
        throw std::runtime_error(
            "authoritative ledger replay refuses rotated log segments; "
            "segment stitching requires a complete manifest");

    // Validate the complete economic/lifecycle ledger before mutating the
    // engine. This prevents a corrupt suffix from leaving a plausible,
    // externally visible prefix of cash/order/position state.
    OrderTracker replay_ledger(
        OrderTracker::default_native_execution_capacity,
        std::isfinite(config_.qty_scale) && config_.qty_scale > 0.0
            ? 1.0 / config_.qty_scale : 0.0);
    const auto require_lifecycle_identity = [&replay_ledger](
        std::uint64_t order_id,
        const std::string& symbol,
        std::chrono::system_clock::time_point timestamp)
        -> const order_ledger_entry&
    {
        const auto* tracked = replay_ledger.find(order_id);
        if (order_id == 0 || symbol.empty() || !tracked
            || replay_ledger.symbol_of(*tracked) != symbol
            || timestamp.time_since_epoch().count() <= 0
            || (tracked->created_ts.time_since_epoch().count() > 0
                && timestamp < tracked->created_ts)
            || (tracked->updated_ts.time_since_epoch().count() > 0
                && timestamp < tracked->updated_ts))
            throw std::runtime_error(
                "authoritative ledger contains invalid lifecycle identity");
        return *tracked;
    };
    while (preflight.has_next())
    {
        auto ev = preflight.next();
        if (!ev)
            break;
        switch (ev->get_type())
        {
        case event_type::order:
        {
            const auto& order = static_cast<const order_event&>(*ev);
            if (!replay_ledger.register_order(order))
                throw std::runtime_error(
                    "authoritative replay contains an invalid or reused order identity");
            replay_ledger.set_status(order.get_order_id(), order_status::open);
            break;
        }
        case event_type::fill:
        {
            const auto& fill = static_cast<const fill_event&>(*ev);
            const auto validation = replay_ledger.validate_fill(
                fill, /*require_exchange_identity=*/false,
                /*require_fill_identity=*/true);
            if (!validation.applied()
                || !replay_ledger.commit_fill(fill, validation))
                throw std::runtime_error(
                    "authoritative ledger contains an invalid economic fill");
            break;
        }
        case event_type::cancel:
        {
            const auto& cancel = static_cast<const cancel_event&>(*ev);
            const auto& tracked = require_lifecycle_identity(
                cancel.get_order_id(), cancel.get_symbol(),
                cancel.get_timestamp());
            if (tracked.status == order_status::open
                || tracked.status == order_status::partially_filled)
                replay_ledger.set_status(
                    cancel.get_order_id(), order_status::cancelled);
            else if (tracked.status != order_status::filled)
                throw std::runtime_error(
                    "authoritative ledger contains an invalid cancel transition");
            break;
        }
        case event_type::rejection:
        {
            const auto& rejection = static_cast<const rejection_event&>(*ev);
            const auto& tracked = require_lifecycle_identity(
                rejection.get_order_id(), rejection.get_symbol(),
                rejection.get_timestamp());
            if (tracked.status != order_status::open
                && tracked.status != order_status::pending)
                throw std::runtime_error(
                    "authoritative ledger contains an invalid rejection transition");
            replay_ledger.set_status(
                rejection.get_order_id(), order_status::rejected);
            break;
        }
        case event_type::funding:
        {
            const auto& funding = static_cast<const funding_event&>(*ev);
            if (funding.get_timestamp().time_since_epoch().count() <= 0
                || funding.get_symbol().empty()
                || !std::isfinite(funding.get_qty_change())
                || funding.get_qty_change() != 0.0
                || !std::isfinite(funding.get_cash_delta())
                || funding.get_cash_delta() == 0.0
                || funding.get_reason() != "FUNDING_FEE")
                throw std::runtime_error(
                    "authoritative ledger contains an invalid funding event");
            break;
        }
        case event_type::amend:
        {
            const auto& amend = static_cast<const amend_event&>(*ev);
            (void)require_lifecycle_identity(
                amend.get_order_id(), amend.get_symbol(),
                amend.get_timestamp());
            const auto validation = replay_ledger.validate_amend(
                amend.get_order_id(), amend.get_symbol(),
                amend.get_new_price(), amend.get_new_quantity());
            if (!validation.applied()
                || !replay_ledger.commit_amend(validation))
                throw std::runtime_error(
                    "authoritative ledger contains an invalid committed amendment");
            break;
        }
        case event_type::market:
        case event_type::signal:
        case event_type::tick:
        case event_type::l2_snapshot:
        case event_type::l2_update:
            break;
        }
    }

    EventReplayer replayer(log_path, /*from_us=*/0, replay_to_us);
    clear_pending_state();
    prepare_mark_prices_for_run(/*symbol_hint=*/16);

    const auto start = std::chrono::high_resolution_clock::now();
    std::size_t event_count = 0;

    std::cout << "\rReplay: applying recorded ledger..." << std::flush;

    while (replayer.has_next())
    {
        auto ev = replayer.next();
        if (!ev)
            break;

        switch (ev->get_type())
        {
        case event_type::market:
        {
            const auto& mkt = static_cast<const market_event&>(*ev);
            last_sim_time_ = mkt.get_timestamp();
            last_decision_ts_ = mkt.get_timestamp() + bar_interval_;   // F-08
            last_mid_price_.store(mkt.get_close(), std::memory_order_release);
            last_mark_symbol_ = mkt.get_symbol();
            {
                std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
                last_mark_prices_[mkt.get_symbol()] =
                    mark_point{mkt.get_close(), mkt.get_timestamp()};
            }
            publish_event(ev);
            analytics_.on_event(ev);
            break;
        }
        case event_type::tick:
        {
            const auto& tick = static_cast<const tick_event&>(*ev);
            last_sim_time_ = tick.get_timestamp();
            last_decision_ts_ = tick.get_timestamp();   // F-08
            last_mid_price_.store(tick.get_price(), std::memory_order_release);
            last_mark_symbol_ = tick.get_symbol();
            {
                std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
                last_mark_prices_[tick.get_symbol()] =
                    mark_point{tick.get_price(), tick.get_timestamp()};
            }
            publish_event(ev);
            analytics_.on_event(ev);
            break;
        }
        case event_type::order:
        {
            const auto& order = static_cast<const order_event&>(*ev);
            // R3: replay must rebuild the authoritative ledger, not just a
            // status flag — the replayed run's risk decisions depend on the
            // same open-order quantities the original run saw.
            if (!order_tracker_.register_order(order))
                throw std::runtime_error(
                    "authoritative replay contains an invalid or reused order identity");
            attribution_->register_order(order);
            order_tracker_.set_status(order.get_order_id(), order_status::open);
            publish_event(ev);
            analytics_.on_event(ev);
            break;
        }
        case event_type::fill:
        {
            auto& fill = static_cast<fill_event&>(*ev);
            bool replay_halt = false;
            if (!fills_->ingest(fill, event_count, replay_halt,
                                fill_context::authoritative_replay))
                throw std::runtime_error(
                    "authoritative ledger contains an invalid economic fill");
            break;
        }
        case event_type::cancel:
        {
            const auto& cancel = static_cast<const cancel_event&>(*ev);
            const auto* tracked = order_tracker_.find(cancel.get_order_id());
            if (cancel.get_order_id() == 0 || cancel.get_symbol().empty()
                || !tracked
                || order_tracker_.symbol_of(*tracked) != cancel.get_symbol()
                || cancel.get_timestamp().time_since_epoch().count() <= 0
                || (tracked->created_ts.time_since_epoch().count() > 0
                    && cancel.get_timestamp() < tracked->created_ts)
                || (tracked->updated_ts.time_since_epoch().count() > 0
                    && cancel.get_timestamp() < tracked->updated_ts)
                || (tracked->status != order_status::open
                    && tracked->status != order_status::partially_filled
                    && tracked->status != order_status::filled))
                throw std::runtime_error(
                    "authoritative ledger contains an invalid cancel transition");
            order_tracker_.set_status(cancel.get_order_id(),
                                      order_status::cancelled);
            if (dashboard_builder_)
                dashboard_builder_->erase_open_order(cancel.get_order_id());
            publish_event(ev);
            analytics_.on_event(ev);
            break;
        }
        case event_type::rejection:
        {
            const auto& rejection = static_cast<const rejection_event&>(*ev);
            const auto* tracked = order_tracker_.find(
                rejection.get_order_id());
            if (rejection.get_order_id() == 0
                || rejection.get_symbol().empty()
                || !tracked
                || order_tracker_.symbol_of(*tracked)
                    != rejection.get_symbol()
                || rejection.get_timestamp().time_since_epoch().count() <= 0
                || (tracked->created_ts.time_since_epoch().count() > 0
                    && rejection.get_timestamp() < tracked->created_ts)
                || (tracked->updated_ts.time_since_epoch().count() > 0
                    && rejection.get_timestamp() < tracked->updated_ts)
                || (tracked->status != order_status::open
                    && tracked->status != order_status::pending))
                throw std::runtime_error(
                    "authoritative ledger contains an invalid rejection transition");
            order_tracker_.set_status(rejection.get_order_id(),
                                      order_status::rejected);
            if (dashboard_builder_)
                dashboard_builder_->erase_open_order(rejection.get_order_id());
            publish_event(ev);
            analytics_.on_event(ev);
            break;
        }
        case event_type::funding:
        {
            const auto& funding = static_cast<const funding_event&>(*ev);
            if (funding.get_timestamp().time_since_epoch().count() <= 0
                || funding.get_symbol().empty()
                || !std::isfinite(funding.get_qty_change())
                || funding.get_qty_change() != 0.0
                || !std::isfinite(funding.get_cash_delta())
                || funding.get_cash_delta() == 0.0
                || funding.get_reason() != "FUNDING_FEE")
                throw std::runtime_error(
                    "authoritative ledger contains an invalid funding event");
            // publish_event is the single Portfolio funding mutation.
            publish_event(ev);
            analytics_.on_event(ev);
            break;
        }
        case event_type::l2_snapshot:
        {
            const auto& snapshot =
                static_cast<const l2_snapshot_event&>(*ev);
            std::array<std::pair<Price, quantity>, kL2SnapshotMaxLevels>
                bids{};
            std::array<std::pair<Price, quantity>, kL2SnapshotMaxLevels>
                asks{};
            auto convert_level = [&](const l2_level& level,
                                     std::pair<Price, quantity>& out) {
                Price px;
                std::uint64_t qty = 0;
                if (!(level.price > 0.0)
                    || !Price::try_from_double(level.price, px)
                    || !tt::quantity_scale::rescale_nonnegative(
                        level.quantity, snapshot.get_quantity_scale(),
                        config_.qty_scale, qty))
                    throw std::runtime_error(
                        "authoritative ledger contains invalid L2 snapshot");
                out = {px, static_cast<quantity>(qty)};
            };
            for (std::size_t i = 0; i < snapshot.bid_count(); ++i)
                convert_level(snapshot.bid(i), bids[i]);
            for (std::size_t i = 0; i < snapshot.ask_count(); ++i)
                convert_level(snapshot.ask(i), asks[i]);

            auto ob = orderbook_registry_.get_or_create(snapshot.get_symbol());
            ob->apply_l2_snapshot(bids.data(), snapshot.bid_count(),
                                  asks.data(), snapshot.ask_count());
            const double bid = ob->best_external_bid_price();
            const double ask = ob->best_external_ask_price();
            const double mark = bid > 0.0 && ask > 0.0
                ? (bid + ask) * 0.5 : (bid > 0.0 ? bid : ask);
            last_sim_time_ = snapshot.get_timestamp();
            last_decision_ts_ = snapshot.get_timestamp();   // F-08
            publish_event(ev);
            analytics_.on_event(ev);
            if (mark > 0.0)
            {
                last_mid_price_.store(mark, std::memory_order_release);
                last_mark_symbol_ = snapshot.get_symbol();
                {
                    std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
                    last_mark_prices_[snapshot.get_symbol()] =
                        mark_point{mark, snapshot.get_timestamp()};
                }
                analytics_.on_mark(snapshot.get_symbol(), mark);
            }
            break;
        }
        case event_type::l2_update:
        {
            const auto& update = static_cast<const l2_update_event&>(*ev);
            Price px;
            std::uint64_t qty = 0;
            if (!(update.get_price() > 0.0)
                || !Price::try_from_double(update.get_price(), px)
                || (update.get_side() != tick_side::bid
                    && update.get_side() != tick_side::ask)
                || !tt::quantity_scale::rescale_nonnegative(
                    update.get_new_quantity(), update.get_quantity_scale(),
                    config_.qty_scale, qty))
                throw std::runtime_error(
                    "authoritative ledger contains invalid L2 update");

            auto ob = orderbook_registry_.get_or_create(update.get_symbol());
            ob->apply_l2_update(
                update.get_side() == tick_side::bid ? side::buy : side::sell,
                px, static_cast<quantity>(qty));
            const double bid = ob->best_external_bid_price();
            const double ask = ob->best_external_ask_price();
            const double mark = bid > 0.0 && ask > 0.0
                ? (bid + ask) * 0.5 : (bid > 0.0 ? bid : ask);
            last_sim_time_ = update.get_timestamp();
            publish_event(ev);
            analytics_.on_event(ev);
            if (mark > 0.0)
            {
                last_mid_price_.store(mark, std::memory_order_release);
                last_mark_symbol_ = update.get_symbol();
                {
                    std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
                    last_mark_prices_[update.get_symbol()] =
                        mark_point{mark, update.get_timestamp()};
                }
                analytics_.on_mark(update.get_symbol(), mark);
            }
            break;
        }
        case event_type::amend:
        {
            const auto& amend = static_cast<const amend_event&>(*ev);
            const auto* tracked = order_tracker_.find(amend.get_order_id());
            if (!tracked
                || order_tracker_.symbol_of(*tracked) != amend.get_symbol()
                || amend.get_timestamp().time_since_epoch().count() <= 0
                || (tracked->created_ts.time_since_epoch().count() > 0
                    && amend.get_timestamp() < tracked->created_ts)
                || (tracked->updated_ts.time_since_epoch().count() > 0
                    && amend.get_timestamp() < tracked->updated_ts)
                || !order_tracker_.amend(
                    amend.get_order_id(), amend.get_new_price(),
                    amend.get_new_quantity()))
                throw std::runtime_error(
                    "authoritative ledger contains an invalid committed amendment");
            publish_event(ev);
            analytics_.on_event(ev);
            break;
        }
        case event_type::signal:
            // Observer/analytics reconstruction only: never apply executable
            // book state or regenerate decisions during ledger replay.
            publish_event(ev);
            analytics_.on_event(ev);
            break;
        }

        ++event_count;
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start).count();

    std::cout << std::endl
              << "Replay complete: " << event_count << " recorded events in "
              << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;
}
