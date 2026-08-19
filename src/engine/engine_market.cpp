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
                               std::uint64_t quantity_scale)
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
        last_mark_prices_[symbol] = l2_mark;
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
    if (router_) router_->on_l2_snapshot(
        symbol, l2_bid_scratch_, l2_ask_scratch_);

    const auto l2_ts = timestamp.time_since_epoch().count() != 0
        ? timestamp : std::chrono::system_clock::now();
    last_sim_time_ = l2_ts;
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
    if (router_) router_->advance_all(l2_ts);
    drain_pending_orders(l2_ts, l2_event_count, l2_halt, symbol);
    if (l2_halt || halt_flag_.load(std::memory_order_acquire))
        return;
    if (l2_mark > 0.0)
        (void)evaluate_exits(symbol, l2_mark, l2_ts,
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
        last_mark_prices_[symbol] = l2_mark;
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
    if (router_) router_->on_l2_update(
        symbol, os, price,
        tt::quantity_scale::to_base(new_qty, quantity_scale));

    const auto l2_ts = timestamp.time_since_epoch().count() != 0
        ? timestamp : std::chrono::system_clock::now();
    last_sim_time_ = l2_ts;
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
    if (router_) router_->advance_all(l2_ts);
    if (l2_mark > 0.0)
        drain_pending_orders(l2_ts, l2_event_count, l2_halt, symbol);
    if (l2_halt || halt_flag_.load(std::memory_order_acquire))
        return;

    // Mid/last price for ExitManager on pure L2 streams (no tick/bar).
    if (l2_mark > 0.0
        && evaluate_exits(symbol, l2_mark, l2_ts,
                          l2_event_count, l2_recv_ns))
        return;

    if (strategy_) {
        sync_strategy_account_equity(*strategy_);
        if (auto o = strategy_->on_l2_update(*ev)) {
            o->set_recv_ns(l2_recv_ns);
            o->set_strategy_name(primary_strategy_name_);
            bool route_halt = false;
            route_order(*o, l2_ts, l2_event_count, route_halt);
            finalize_strategy_route(*strategy_, primary_strategy_name_, *o, route_halt);
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
            bool route_halt = false;
            route_order(*o, l2_ts, l2_event_count, route_halt);
            finalize_strategy_route(*s, additional_strategy_names_[i], *o, route_halt);
            if (route_halt || halt_flag_.load(std::memory_order_acquire))
                return;
        }
    }
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

        if (evaluate_exits(mkt.get_symbol(),
                           mkt.get_open(), mkt.get_low(), mkt.get_high(), mkt.get_close(),
                           ts, event_count, mkt.get_recv_ns()))
            return;

        sync_strategy_account_equity(*s);
        if (auto o = s->on_market(mkt))
        {
            o->set_recv_ns(mkt.get_recv_ns());
            o->set_strategy_name(additional_strategy_names_[i]);
            bool halt = false;
            route_order(*o, ts, event_count, halt);
            finalize_strategy_route(*s, additional_strategy_names_[i], *o, halt);
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

        if (evaluate_exits(te.get_symbol(), te.get_price(), ts,
                           event_count, te.get_recv_ns()))
            return;

        sync_strategy_account_equity(*s);
        if (auto o = s->on_tick(te))
        {
            o->set_recv_ns(te.get_recv_ns());
            o->set_strategy_name(additional_strategy_names_[i]);
            bool halt = false;
            route_order(*o, ts, event_count, halt);
            finalize_strategy_route(*s, additional_strategy_names_[i], *o, halt);
            if (halt || halt_flag_.load(std::memory_order_acquire)) return;
        }
    }
}

double engine::marked_account_equity(std::string_view current_symbol,
                                     double current_mark) const
{
    double equity = portfolio_.get_cash();
    const auto& positions = portfolio_.get_positions();
    if (positions.empty())
        return equity;
    if (positions.size() == 1 && current_mark > 0.0)
    {
        const auto& [symbol, position] = *positions.begin();
        if (symbol == current_symbol)
            return equity + position.qty * current_mark;
    }
    std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
    for (const auto& [symbol, position] : positions)
    {
        if (std::abs(position.qty) <= 1e-12)
            continue;
        double position_mark = 0.0;
        if (symbol == current_symbol && current_mark > 0.0)
            position_mark = current_mark;
        else if (auto it = last_mark_prices_.find(symbol);
                 it != last_mark_prices_.end() && it->second > 0.0)
            position_mark = it->second;
        else
            return std::numeric_limits<double>::quiet_NaN();
        equity += position.qty * position_mark;
    }
    return equity;
}

void engine::sync_strategy_account_equity(IStrategy& strategy) const
{
    const double equity = marked_account_equity(
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
        unwind_positions(event_count);

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
    last_mid_price_.store(mkt.get_open(), std::memory_order_release);
    last_mark_symbol_ = mkt.get_symbol();
    { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[mkt.get_symbol()] = mkt.get_open(); }

    // Drain delayed orders at open mid for each order's symbol (not the
    // event symbol alone — multi-symbol pending must not walk the wrong book).
    bool halt = false;
    drain_pending_orders(timestamp, event_count, halt, mkt.get_symbol());
    // Risk halt (or other terminal) during pending drain: do not continue into
    // strategy / route_order on this bar (was previously loop-scoped only).
    if (halt || halt_flag_.load(std::memory_order_acquire))
        return;

    last_mid_price_.store(mkt.get_close(), std::memory_order_release);
    last_mark_symbol_ = mkt.get_symbol();
    { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[mkt.get_symbol()] = mkt.get_close(); }

    {
        // Single stop pass (EL-STREAM-DOUBLE-STOPS): matches batch run().
        // Stops + bar-range sweep before MM/provider fills.
        bool halt = false;
        check_pending_stops(mkt.get_symbol(), mkt.get_open(),
                            mkt.get_high(), mkt.get_low(),
                            timestamp, event_count, halt);
        swept_volume = sweep_resting_limits(
            mkt.get_symbol(), mkt.get_low(), mkt.get_high(),
            timestamp, event_count, halt, bar_volume);
        // Match tick/history paths: do not generate new MM/provider fills
        // after a terminal halt on this bar.
        if (halt || halt_flag_.load(std::memory_order_acquire))
            return;
    }

    auto ob = orderbook_registry_.get_or_create(mkt.get_symbol());
    if (!mm_worker_ &&
        !l2_seeded_symbols_.count(mkt.get_symbol()))
    {
        auto mm_trades = market_maker_.replenish(
            ob, last_mid_price_.load(std::memory_order_relaxed));
        bool halt = false;
        deliver_mm_book_trades(mkt.get_symbol(), mm_trades,
                               timestamp, event_count, halt);
    }

    // Paper maker-queue: synthetic trade at bar close (lossy, matches shadow bar).
    {
        bool halt_paper = false;
        feed_paper_trade_and_drain(mkt.get_symbol(), mkt.get_close(),
                                   std::max(0.0, bar_volume - swept_volume),
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
        drain_async_submit_results(provider_adapter.get());
        std::vector<fill_event> provider_fills;
        if (provider_adapter && provider_adapter->poll_fills(provider_fills))
        {
            for (auto& f : provider_fills)
            {
                if (config_.mode == engine_mode::shadow)
                {
                    if (shadow_tracker_)
                        shadow_tracker_->on_exchange_fill(f);

                    if (exchange_portfolio_.has_value())
                    {
                        fills_->stamp_fill_attribution(f);
                        exchange_portfolio_->on_fill(f, f.get_opener_order_id(),
                                                   f.get_strategy_name());
                    }
                    if (exchange_analytics_.has_value())
                    {
                        // Feed to second analytics for equity curve / metrics
                        auto fill_ptr = acquire_pooled(fill_pool_,f);
                        exchange_analytics_->on_event(fill_ptr);
                    }
                    continue;
                }
                // Live/async fills: full canonical pipeline (risk + tracker + audit).
                bool fill_halt = false;
                if (!fills_->handle_fill(f, event_count, fill_halt))
                    return;
            }
        }
    }

    // Stops already evaluated once above (EL-STREAM-DOUBLE-STOPS).
    // Canonical order continues: exits → strategy → route.

    auto mkt_ptr = acquire_pooled(market_pool_,mkt);
    log_event(mkt);
    publish_event(mkt_ptr);
    if (!config_.is_threaded())
        analytics_.on_event(mkt_ptr);
    else
        analytics_.on_mark(mkt.get_symbol(), mkt.get_close());
    event_count++;

    if (evaluate_exits(mkt.get_symbol(),
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
        bool route_halt = false;
        route_order(*order_opt, timestamp, event_count, route_halt);
        finalize_strategy_route(*strategy_, primary_strategy_name_, *order_opt, route_halt);
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
        unwind_positions(event_count);

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
    last_mid_price_.store(rec.price, std::memory_order_release);
    last_mark_symbol_ = rec.symbol;
    { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[rec.symbol] = rec.price; }

    {
        DEBUG_STAGE(stage_timer_, mm_replenish);
        auto ob = orderbook_registry_.get_or_create(rec.symbol);
        if (!l2_seeded_symbols_.count(rec.symbol))
        {
            auto mm_trades = market_maker_.replenish(
                ob, last_mid_price_.load(std::memory_order_relaxed));
            bool halt = false;
            deliver_mm_book_trades(rec.symbol, mm_trades,
                                   rec.timestamp, event_count, halt);
        }
    }

    // Paper maker-queue: real tick print advances QueueAware size_ahead.
    {
        bool halt_paper = false;
        feed_paper_trade_and_drain(rec.symbol, rec.price,
                                   tick_qty,
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
                                       rec.timestamp);
        }

        drain_venue_bracket_meta();
        drain_async_submit_results(provider_adapter.get());
        std::vector<fill_event> provider_fills;
        if (provider_adapter && provider_adapter->poll_fills(provider_fills))
        {
            for (auto& f : provider_fills)
            {
                if (config_.mode == engine_mode::shadow)
                {
                    if (shadow_tracker_)
                        shadow_tracker_->on_exchange_fill(f);

                    if (exchange_portfolio_.has_value())
                    {
                        fills_->stamp_fill_attribution(f);
                        exchange_portfolio_->on_fill(f, f.get_opener_order_id(),
                                                   f.get_strategy_name());
                    }
                    if (exchange_analytics_.has_value())
                    {
                        // Feed to second analytics for equity curve / metrics
                        auto fill_ptr = acquire_pooled(fill_pool_,f);
                        exchange_analytics_->on_event(fill_ptr);
                    }
                    continue;
                }
                // Live/async fills: full canonical pipeline (risk + tracker + audit).
                bool fill_halt = false;
                if (!fills_->handle_fill(f, event_count, fill_halt))
                    return;
            }
        }
    }

    bool halt = false;

    {
        DEBUG_STAGE(stage_timer_, pending_drain);
        drain_pending_orders(rec.timestamp, event_count, halt, rec.symbol);
    }
    if (halt || halt_flag_.load(std::memory_order_acquire)) return;

    {
        DEBUG_STAGE(stage_timer_, stop_check);
        check_pending_stops(rec.symbol, rec.price, rec.price, rec.price,
                            rec.timestamp, event_count, halt);
    }
    if (halt || halt_flag_.load(std::memory_order_acquire)) return;

    auto tick_ptr = acquire_pooled(tick_pool_,te);
    log_event(te);
    {
        DEBUG_STAGE(stage_timer_, ring_publish);
        publish_event(tick_ptr);
    }
    if (!config_.is_threaded())
        analytics_.on_event(tick_ptr);
    else
        analytics_.on_mark(rec.symbol, rec.price);
    event_count++;

    if (evaluate_exits(rec.symbol, rec.price, rec.timestamp,
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
        route_order(*order_opt, rec.timestamp, event_count, halt);
        finalize_strategy_route(*strategy_, primary_strategy_name_, *order_opt, halt);
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
                set_strategy(StrategyFactory::create(new_strat, params));
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
        drain_final_pending(event_count, halt);
        cancel_day_orders();
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
                set_strategy(StrategyFactory::create(new_strat, params));
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
        drain_final_pending(event_count, halt);
        cancel_day_orders();
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
                                  e.quantity_scale);
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
        drain_final_pending(event_count, halt);
        cancel_day_orders();
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
        if (!config_.is_threaded())
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
        last_mid_price_.store(tick.price, std::memory_order_release);
        last_mark_symbol_ = tick.symbol;
        { std::lock_guard<std::mutex> lk(last_mark_prices_mu_); last_mark_prices_[tick.symbol] = tick.price; }

        // Latency-gated cancel windows need clock advance on the tick path too.
        if (router_) router_->advance_all(tick.timestamp);

        {
            DEBUG_STAGE(stage_timer_, mm_replenish);
            auto ob = orderbook_registry_.get_or_create(tick.symbol);
            if (!l2_seeded_symbols_.count(tick.symbol))
            {
                auto mm_trades = market_maker_.replenish(
                    ob, last_mid_price_.load(std::memory_order_relaxed));
                deliver_mm_book_trades(tick.symbol, mm_trades, tick.timestamp,
                                       event_count, halt_requested);
            }
        }

        {
            DEBUG_STAGE(stage_timer_, pending_drain);
            drain_pending_orders(tick.timestamp, event_count, halt_requested,
                                 tick.symbol);
        }
        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

        {
            DEBUG_STAGE(stage_timer_, stop_check);
            check_pending_stops(tick.symbol, tick.price, tick.price,
                                tick.price, tick.timestamp, event_count,
                                halt_requested);
        }
        if (halt_requested || halt_flag_.load(std::memory_order_acquire)) break;

        feed_paper_trade_and_drain(tick.symbol, tick.price,
                                   tt::quantity_scale::to_base(
                                       tick.quantity, tick.quantity_scale),
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
        if (!config_.is_threaded())
            analytics_.on_event(tick_ptr);
        else
            analytics_.on_mark(tick.symbol, tick.price);
        event_count++;

        if (evaluate_exits(tick.symbol, tick.price, tick.timestamp,
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
            route_order(*order_opt, tick.timestamp, event_count, halt_requested);
            if (strategy_)
                finalize_strategy_route(*strategy_, primary_strategy_name_, *order_opt,
                                        halt_requested);
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

    drain_final_pending(event_count, halt_requested);
    cancel_day_orders();

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

    EventReplayer replayer(log_path, /*from_us=*/0, replay_to_us);
    if (replayer.file_version() != EVENT_LOG_FILE_VERSION)
        throw std::runtime_error(
            "authoritative ledger replay requires a current v2 event log; "
            "legacy logs remain inspection-only");
    if (!replayer.file_finalized())
        throw std::runtime_error(
            "authoritative ledger replay requires a finalized event log; "
            "an in-progress or crash-truncated prefix is inspection-only");
    if (replayer.file_segmented())
        throw std::runtime_error(
            "authoritative ledger replay refuses rotated log segments; "
            "segment stitching requires a complete manifest");
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
            last_mid_price_.store(mkt.get_close(), std::memory_order_release);
            last_mark_symbol_ = mkt.get_symbol();
            {
                std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
                last_mark_prices_[mkt.get_symbol()] = mkt.get_close();
            }
            publish_event(ev);
            analytics_.on_event(ev);
            break;
        }
        case event_type::tick:
        {
            const auto& tick = static_cast<const tick_event&>(*ev);
            last_sim_time_ = tick.get_timestamp();
            last_mid_price_.store(tick.get_price(), std::memory_order_release);
            last_mark_symbol_ = tick.get_symbol();
            {
                std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
                last_mark_prices_[tick.get_symbol()] = tick.get_price();
            }
            publish_event(ev);
            analytics_.on_event(ev);
            break;
        }
        case event_type::order:
        {
            const auto& order = static_cast<const order_event&>(*ev);
            register_order_meta(order);
            order_tracker_.set_status(order.get_order_id(), order_status::open);
            publish_event(ev);
            analytics_.on_event(ev);
            break;
        }
        case event_type::fill:
        {
            auto& fill = static_cast<fill_event&>(*ev);
            fills_->stamp_fill_attribution(fill);
            order_tracker_.set_status(
                fill.get_order_id(),
                fill.is_partial() ? order_status::partially_filled
                                  : order_status::filled);
            if (dashboard_builder_)
            {
                dashboard_builder_->cache_fill(fill);
                if (fill.is_partial())
                    dashboard_builder_->update_open_order_status(
                        fill.get_order_id(), "partial");
                else
                    dashboard_builder_->erase_open_order(fill.get_order_id());
            }
            portfolio_.on_fill(fill, fill.get_opener_order_id(),
                               fill.get_strategy_name());
            publish_event(ev);
            analytics_.on_event(ev);
            break;
        }
        case event_type::cancel:
        {
            const auto& cancel = static_cast<const cancel_event&>(*ev);
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
            order_tracker_.set_status(rejection.get_order_id(),
                                      order_status::rejected);
            if (dashboard_builder_)
                dashboard_builder_->erase_open_order(rejection.get_order_id());
            publish_event(ev);
            analytics_.on_event(ev);
            break;
        }
        case event_type::funding:
            // publish_event is the single Portfolio funding mutation.
            publish_event(ev);
            analytics_.on_event(ev);
            break;
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
            publish_event(ev);
            analytics_.on_event(ev);
            if (mark > 0.0)
            {
                last_mid_price_.store(mark, std::memory_order_release);
                last_mark_symbol_ = snapshot.get_symbol();
                {
                    std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
                    last_mark_prices_[snapshot.get_symbol()] = mark;
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
                    last_mark_prices_[update.get_symbol()] = mark;
                }
                analytics_.on_mark(update.get_symbol(), mark);
            }
            break;
        }
        case event_type::signal:
        case event_type::amend:
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
