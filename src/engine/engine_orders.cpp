// Engine order lifecycle: submission (process_order), routing, cancel/modify,
// resting-order triggering (stops, sweeps, exits), and order/strategy attribution.
// Extracted mechanically from engine.cpp (Phase 1 TU split); behavior unchanged.
#include "engine.h"
#include "execution/latency_model.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

bool engine::process_order(const std::shared_ptr<order_event>& o,
                           std::size_t& event_count,
                           bool& halt_requested)
{
    // S3: process-wide halt is terminal for ALL submit call sites, including
    // check_pending_stops / pending drains that bypass route_order.
    if (halt_flag_.load(std::memory_order_acquire))
    {
        halt_requested = true;
        return false;
    }

    // ========================================================================
    // CANONICAL HOT-PATH ORDERING (Phase 3 deepdive cleanup)
    // This documents the enforced sequence for order + fill processing.
    // All run_* paths (bar/tick/stream/replay), evaluate_exits, unwind, etc.
    // should follow this for consistent per-lot state, shadow divergence,
    // publish to rings/workers, and cache updates.
    //
    // 1. Venue pre-trade risk (FuturesRiskCheck / risk_check_) — reject only.
    // 2. RiskManager pre-order check (can halt).
    // 3. route_order (assigns id, register_order_meta for opener/strategy,
    //    instrument spec, stop pending, or submit).
    // 4. adapter->submit_order (paper or live); also submit to shadow provider
    //    adapter for dual tracking.
    // 5. adapter->poll_fills → for each fill:
    //      - stamp_fill_attribution (rich opener/strategy from meta or fe)
    //      - order_tracker / cache status
    //      - log + publish order status if needed
    //      - portfolio_.on_fill (rich, with opener/strategy)  [core lot update]
    //      - dispatch_fill_to_strategy
    //      - adverse_selection_.on_fill
    //      - exit_manager_.on_fill (rich)  [arm/cancel brackets per opener]
    //      - risk_manager_.on_fill
    //      - QuestDB record_fill (rich)
    //      - notify_position_change_all (multi-lot aware)
    //      - publish_event(fill) + analytics_.on_event
    //      - shadow_tracker on_sim (if shadow)
    //      - post_fill risk check (can halt + unwind)
    // 6. Shadow dual: separate poll of provider's exchange_adapter fills →
    //      shadow_tracker on_exchange + exchange_portfolio on_fill (rich)
    //      + exchange_analytics
    // 7. evaluate_exits (price or bar) — can emit closes that recurse via
    //      route_order/process_order (keeps lot/opener discipline).
    // 8. Cache updates for dashboard (open_orders, recent_fills) + rings
    //      publish for workers (after core state for snapshot coherence).
    //
    // Invariants: order_meta_ registered before any fill can reference it.
    // L2 updates reach queue models before trades (via apply_l2 before
    // on_trade in adapters). No new allocs/JSON on hot path. Multi-lot uses
    // opener_order_id discipline; single-lot may use bulk cancel in notify.
    // ========================================================================

    {
        double order_mark = std::numeric_limits<double>::quiet_NaN();
        const double latest_mark =
            last_mid_price_.load(std::memory_order_relaxed);
        if (last_mark_symbol_ == o->get_symbol()
            && std::isfinite(latest_mark) && latest_mark > 0.0)
        {
            order_mark = latest_mark;
        }
        else
        {
            std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
            if (auto it = last_mark_prices_.find(o->get_symbol());
                it != last_mark_prices_.end()
                && std::isfinite(it->second) && it->second > 0.0)
                order_mark = it->second;
        }
        const double marked_equity = marked_account_equity(
            o->get_symbol(), order_mark);

        // Venue-specific pre-trade check (futures notional / leverage /
        // liquidation distance) runs first. Refusals here are pure
        // rejections — no halt semantics, since these caps describe
        // the operator's prudent-trading envelope, not a market-wide
        // risk-of-ruin trigger.
        if (risk_check_)
        {
            auto vd = risk_check_->evaluate_with_account_equity(
                *o, portfolio_, order_mark, marked_equity);
            if (!vd.allow)
            {
                const std::string reason_str =
                    "venue risk check refused: " + vd.reason;
                auto rej = acquire_pooled(rejection_pool_,
                    o->get_timestamp(), o->get_symbol(),
                    o->get_order_id(), reason_str);
                log_event(*rej);
                publish_event(rej);
                // Migrated to sink (PR-04)
                audit_sink_->record_order_submitted(*o, "rejected");
                // Use stack buffer for audit detail to avoid string temp on this path (pooled still needs string)
                char venue_reason[128];
                std::snprintf(venue_reason, sizeof(venue_reason), "venue risk check refused: %s", vd.reason.c_str());
                audit_sink_->record_rejection(*o, "venue_risk_reject", venue_reason);
                order_tracker_.set_status(o->get_order_id(),
                                          order_status::rejected);
                if (dashboard_builder_) dashboard_builder_->erase_open_order(o->get_order_id());
                // Reject, not halt — engine continues. The cap describes
                // what this operator considers prudent, not a market-wide
                // risk-of-ruin condition that should stop everything.
                return true;
            }
        }

        auto snap = analytics_.risk_view();
        snap.equity = std::isfinite(marked_equity) ? marked_equity : 0.0;
        // This candidate has not been transitioned into the active lifecycle
        // yet, so the tracker count is the exact pre-trade capacity.
        auto existing_active_orders = order_tracker_.active_count();
        // A pending stop already owns exactly one slot before it fires. Its
        // conversion to a market/limit order is not a second candidate.
        if (order_tracker_.is_active(o->get_order_id()) &&
            existing_active_orders > 0)
            --existing_active_orders;
        o->set_pretrade_open_order_count(existing_active_orders);
        auto action = risk_manager_.check_order(*o, portfolio_, snap,
                                                existing_active_orders);
        // Backtest research only: portfolio risk breaches reject the trade —
        // never stop the market replay. Live/shadow keep terminal halt even if
        // the soft flag was left true by misconfiguration.
        const bool soft_pf = config_.risk_soft_portfolio_limits
            && config_.mode == engine_mode::backtest;
        if (action == risk_action::halt && soft_pf)
            action = risk_action::reject;
        if (action == risk_action::halt || action == risk_action::reject)
        {
            const char* reason = (action == risk_action::halt)
                ? "risk limit breached - engine halted"
                : (soft_pf
                       ? "order rejected by risk manager (soft portfolio limits)"
                       : "order rejected by risk manager");

            auto rej = acquire_pooled(rejection_pool_,
                o->get_timestamp(), o->get_symbol(), o->get_order_id(), reason);
            log_event(*rej);
            publish_event(rej);

            // Migrated to sink (PR-04)
            audit_sink_->record_order_submitted(*o, "rejected");
            audit_sink_->record_rejection(*o,
                (action == risk_action::halt) ? "risk_halt" : "risk_reject",
                reason);
            audit_sink_->record_event(
                "risk_decision",
                o->get_symbol().c_str(),
                o->get_strategy_name().c_str(),
                o->get_order_id(),
                (action == risk_action::halt) ? "halt" : "reject",
                reason,
                "{}"
            );

            order_tracker_.set_status(o->get_order_id(), order_status::rejected);
            if (dashboard_builder_) dashboard_builder_->erase_open_order(o->get_order_id());
            if (action == risk_action::halt)
            {
                // Terminal process-wide halt: set halt_flag_ so DataBridge,
                // L2 dispatch, and run loops all stop — not just the local
                // halt_requested out-param (S3: halt is write-once terminal).
                if (config_.risk_unwind)
                    unwind_positions(event_count);
                trigger_halt(reason);
                halt_requested = true;
                return false;
            }
            return true;
        }
    }

    auto adapter = router_->resolve_adapter(o->get_symbol());
    const bool async_submit = router_->is_async_submit(adapter.get());

    order_tracker_.set_status(o->get_order_id(),
        async_submit ? order_status::pending : order_status::open);
    if (dashboard_builder_) {
        dashboard_builder_->cache_open_order(*o);
        if (async_submit)
            dashboard_builder_->update_open_order_status(o->get_order_id(), "submit_pending");
    }
    log_event(*o);
    if (halt_flag_.load(std::memory_order_acquire))
    {
        order_tracker_.set_status(o->get_order_id(), order_status::rejected);
        if (dashboard_builder_)
            dashboard_builder_->erase_open_order(o->get_order_id());
        halt_requested = true;
        return false;
    }
    publish_event(o);
    analytics_.on_event(o);

    // Migrated to sink (PR-04)
    audit_sink_->record_order_submitted(*o, "pending");
    if (!async_submit)
    {
        audit_sink_->record_status_transition(o->get_order_id(),
            order_status::pending, order_status::open);
    }
    audit_sink_->record_event(
        "order_intent",
        o->get_symbol().c_str(),
        o->get_strategy_name().c_str(),
        o->get_order_id(),
        "info",
        "order generated by strategy",
        "{}"
    );

    adapter->set_mid_price(last_mid_price_.load(std::memory_order_relaxed));
    adapter->set_l2_seeded(l2_seeded_symbols_.count(o->get_symbol()) > 0);

    router_->submit(*o, adapter.get());

    drain_async_submit_results(adapter.get());

    if (config_.mode == engine_mode::shadow && config_.provider)
    {
        auto exchange_adapter = config_.provider->get_execution_adapter();
        if (exchange_adapter)
        {
            exchange_adapter->submit_order(*o);
            drain_async_submit_results(exchange_adapter.get());
        }
    }

    if (!fills_->process_adapter_fills(adapter, event_count, halt_requested))
        return false;

    if (config_.mode == engine_mode::shadow && config_.provider)
    {
        auto exchange_adapter = config_.provider->get_execution_adapter();
        if (exchange_adapter)
        {
            drain_async_submit_results(exchange_adapter.get());
            std::vector<fill_event> exchange_fills;
            if (exchange_adapter->poll_fills(exchange_fills))
            {
                for (auto& ef : exchange_fills)
                {
                    fills_->stamp_fill_attribution(ef);

                    const uint64_t e_opener = ef.get_opener_order_id();
                    const std::string& e_strat = ef.get_strategy_name();

                    if (shadow_tracker_)
                        shadow_tracker_->on_exchange_fill(ef);

                    if (exchange_portfolio_.has_value())
                    {
                        exchange_portfolio_->on_fill(ef, e_opener, e_strat);
                    }
                }
            }
        }
    }

    event_count++;
    return true;
}

void engine::deliver_mm_book_trades(const std::string& symbol, const trades& trs,
                                    const std::chrono::system_clock::time_point& ts,
                                    std::size_t& event_count, bool& halt_requested)
{
    if (trs.empty())
        return;
    // Only adapters that already exist can hold resting strategy orders;
    // don't create one just to deliver MM-vs-MM crossings.
    // Virtual dispatch: LocalBookAdapter records fills; HybridExecutor
    // forwards to its inner book adapter; live bridges no-op.
    auto it = execution_adapters_.find(symbol);
    if (it == execution_adapters_.end() || !it->second)
        return;
    it->second->on_book_trades(trs, ts);
    fills_->process_adapter_fills(it->second, event_count, halt_requested);
}

bool engine::cancel_order(const std::string& symbol, uint64_t order_id,
                          const std::string& reason)
{
    auto adapter = router_->resolve_adapter(symbol);

    drain_async_submit_results(adapter.get());
    if (halt_flag_.load(std::memory_order_acquire))
        return false;

    bool cancelled = adapter->cancel_order(order_id);

    if (cancelled && adapter->supports_async_submit())
    {
        pending_cancels_[order_id] = pending_cancel_meta{symbol, reason};
        if (dashboard_builder_) dashboard_builder_->update_open_order_status(order_id, "cancel_pending");
        return true;
    }

    if (!cancelled)
    {
        auto it = std::remove_if(pending_stops_.begin(), pending_stops_.end(),
            [order_id](const std::shared_ptr<order_event>& o) {
                return o->get_order_id() == order_id;
            });
        if (it != pending_stops_.end())
        {
            pending_stops_.erase(it, pending_stops_.end());
            cancelled = true;
        }
    }

    if (cancelled)
    {
        order_tracker_.set_status(order_id, order_status::cancelled);
        if (dashboard_builder_) dashboard_builder_->erase_open_order(order_id);
        // Prefer last sim time (EL-CANCEL-WALLCLOCK); wall clock only if no event yet.
        const auto cancel_ts = (last_sim_time_.time_since_epoch().count() != 0)
            ? last_sim_time_
            : std::chrono::system_clock::now();
        auto cancel_ev = acquire_pooled(cancel_pool_,
            cancel_ts, symbol, order_id, reason);
        log_event(*cancel_ev);
        publish_event(cancel_ev);
        if (!config_.is_threaded())
            analytics_.on_event(cancel_ev);
        // Unconditional via audit_sink (replaces questdb guard + #ifdef).
        audit_sink_->record_cancellation(order_id, symbol.c_str(),
            lookup_strategy_name(order_id).c_str(),
            reason.empty() ? "manual" : reason.c_str());
        audit_sink_->record_status_transition(order_id,
            order_status::open, order_status::cancelled, reason.empty() ? nullptr : reason.c_str());
    }

    return cancelled;
}

bool engine::modify_order(const std::string& symbol, uint64_t order_id,
                          double new_price, double new_qty)
{
    // S3: no amend of resting live orders after process-wide terminal halt
    // or operator pause (new risk / size changes must not sneak through).
    if (halt_flag_.load(std::memory_order_acquire) ||
        pause_all_.load(std::memory_order_acquire))
        return false;

    auto adapter = router_->resolve_adapter(symbol);
    bool modified = adapter->modify_order(order_id, new_price, new_qty);

    if (modified)
    {
        const auto now = (last_sim_time_.time_since_epoch().count() != 0)
            ? last_sim_time_
            : std::chrono::system_clock::now();
        auto amend_ev = acquire_pooled(amend_pool_,
            now, symbol, order_id, new_price, new_qty);
        log_event(*amend_ev);
        publish_event(amend_ev);
        if (!config_.is_threaded())
            analytics_.on_event(amend_ev);
        // Unconditional via audit_sink (replaces questdb guard + #ifdef).
        // Engine doesn't preserve old price/qty cleanly here; log zeros
        // and rely on the orders/order_status tables for history.
        audit_sink_->record_amendment(order_id, symbol.c_str(),
            /*old_price=*/0.0, new_price,
            /*old_qty=*/0.0, new_qty, now);
    }

    return modified;
}

void engine::unwind_positions(std::size_t& event_count)
{
    // Snapshot before iterating — each fill mutates positions_.
    std::vector<std::pair<std::string, double>> to_close;
    to_close.reserve(portfolio_.get_positions().size());
    for (const auto& [symbol, pos] : portfolio_.get_positions())
    {
        if (std::abs(pos.qty) >= 1e-12)
            to_close.emplace_back(symbol, pos.qty);
    }

    for (const auto& [symbol, qty] : to_close)
    {
        // Sign-aware flatten — shorts need market BUY, not SELL.
        const order_side close_side = (qty > 0.0)
            ? order_side::sell : order_side::buy;
        const double close_qty = std::abs(qty);

        auto now = std::chrono::system_clock::now();
        auto close_order = acquire_pooled(order_pool_,order_event(
            now, symbol, order_type::market, close_side,
            close_qty, last_mid_price_.load(std::memory_order_relaxed)));
        close_order->set_order_id(OrderIdGenerator::next());
        close_order->set_strategy_name("risk_unwind");

        order_tracker_.set_status(close_order->get_order_id(), order_status::open);
        if (dashboard_builder_) dashboard_builder_->cache_open_order(*close_order);
        log_event(*close_order);
        publish_event(close_order);
        analytics_.on_event(close_order);

        // Unconditional via audit_sink (replaces questdb guard + #ifdef).
        audit_sink_->record_order_submitted(*close_order, "pending");
        audit_sink_->record_status_transition(close_order->get_order_id(),
            order_status::pending, order_status::open, "risk_unwind");

        auto adapter = router_->resolve_adapter(symbol);
        adapter->set_mid_price(last_mid_price_.load(std::memory_order_relaxed));
        adapter->set_l2_seeded(l2_seeded_symbols_.count(symbol) > 0);

        router_->submit(*close_order, adapter.get());

        drain_async_submit_results(adapter.get());

        std::vector<fill_event> fills;
        if (router_->poll_fills(adapter.get(), fills))
        {
            bool unwind_halt = false;
            for (auto& f : fills)
            {
                // Already in halt/unwind — skip post-fill re-halt.
                (void)fills_->handle_fill(f, event_count, unwind_halt,
                                          /*run_post_fill_risk=*/false,
                                          /*mark_shadow_sim=*/false,
                                          "risk_unwind");
            }
        }
    }
}

const instrument_spec* engine::resolve_instrument_spec(const std::string& symbol)
{
    return instrument_spec_cache_ ? instrument_spec_cache_->resolve_instrument_spec(symbol) : nullptr;
}

bool engine::apply_instrument_spec(order_event& o, const instrument_spec& spec) const
{
    // Delegate (no duplication); cache owns impl + cache map.
    return instrument_spec_cache_ ? instrument_spec_cache_->apply_instrument_spec(o, spec) : true;
}

bool engine::route_order(order_event& order,
                         const std::chrono::system_clock::time_point& sim_time,
                         std::size_t& event_count, bool& halt_requested,
                         bool anchor_immediate)
{
    // Terminal halt gate: refuse new submits even if a call site forgot to
    // re-check halt_flag_ (e.g. L2 multi-strategy loop after primary halt).
    if (halt_flag_.load(std::memory_order_acquire))
    {
        halt_requested = true;
        return false;
    }

    // Operator-pause gate: intercept here so every strategy call site is
    // covered by one branch. Strategies still run (so analytics + lots
    // stay live for fills already in flight), but no new orders reach
    // the venue. The intent's order_id stays 0; finalize_strategy_route
    // drains exit intents and resyncs optimistic position gates.
    if (pause_all_.load(std::memory_order_acquire))
    {
        (void)sim_time; (void)event_count; (void)halt_requested;
        return true;
    }

    order.set_order_id(OrderIdGenerator::next());
    // Canonical step: register_order_meta before any submit or potential fill.
    // This populates opener/strategy so stamp_fill_attribution and rich
    // on_fill paths have the data (critical for per-lot and multi-lot).
    register_order_meta(order);

    if (auto* spec = resolve_instrument_spec(order.get_symbol()))
    {
        if (!apply_instrument_spec(order, *spec))
        {
            const char* reason = "order rejected by venue filter (min_qty/min_notional)";
            auto rej = acquire_pooled(rejection_pool_,
                order.get_timestamp(), order.get_symbol(),
                order.get_order_id(), reason);
            log_event(*rej);
            publish_event(rej);
            // Unconditional via audit_sink (replaces questdb guard + #ifdef + dead total_rejections_).
            audit_sink_->record_order_submitted(order, "rejected");
            audit_sink_->record_rejection(order, "venue_filter", reason);
            order_tracker_.set_status(order.get_order_id(), order_status::rejected);
            (void)event_count;
            (void)halt_requested;
            return true;
        }
    }

    // Reserve the authoritative lifecycle slot before an order can be queued
    // by latency/bar-delay or staged as a stop. This is the sole capacity
    // check at route time; process_order subtracts this candidate when it
    // performs the remaining venue and portfolio checks.
    const auto existing_active_orders = order_tracker_.active_count();
    if (risk_manager_.open_order_limit_reached(existing_active_orders))
    {
        const char* reason = "order rejected by risk manager (max open orders)";
        auto rej = acquire_pooled(rejection_pool_, order.get_timestamp(),
            order.get_symbol(), order.get_order_id(), reason);
        log_event(*rej);
        publish_event(rej);
        audit_sink_->record_order_submitted(order, "rejected");
        audit_sink_->record_rejection(order, "risk_reject", reason);
        order_tracker_.set_status(order.get_order_id(), order_status::rejected);
        return true;
    }
    order.set_pretrade_open_order_count(existing_active_orders);
    order_tracker_.set_status(order.get_order_id(), order_status::pending);

    if (order.get_order_type() == order_type::stop ||
        order.get_order_type() == order_type::stop_limit)
    {
        // A staged stop occupies capacity while it can still execute. Its
        // later conversion keeps the same id and therefore the same slot.
        order_tracker_.set_status(order.get_order_id(), order_status::pending);
        pending_stops_.push_back(acquire_pooled(order_pool_,order));
        return true;
    }

    if (anchor_immediate)
    {
        // Bracket fire: fill where it triggered. Re-center the synthetic
        // book at the anchored fire price (SL/TP level or gap open) within
        // the trigger bar and execute immediately — deferring through
        // execution_bar_delay would discard the fire price and fill at
        // wherever the next bar happens to open. Same convention as the
        // native stop path in check_pending_stops.
        const double ref = order.get_price();
        const double bar_mid = last_mid_price_;
        if (ref > 0.0 && ref != bar_mid)
        {
            auto ob = orderbook_registry_.get_or_create(order.get_symbol());
            if (!mm_worker_ &&
                !l2_seeded_symbols_.count(order.get_symbol()))
            {
                auto mm_trades = market_maker_.replenish(
                    ob, ref, /*update_history=*/false);
                deliver_mm_book_trades(order.get_symbol(), mm_trades,
                                       sim_time, event_count, halt_requested);
            }
            last_mid_price_ = ref;
        }
        order.set_earliest_eligible_ts(sim_time);
        auto order_ptr = acquire_pooled(order_pool_,order);
        if (order.get_tif() == time_in_force::day)
            day_order_ids_.push_back({order.get_symbol(), order.get_order_id()});
        const bool ok = process_order(order_ptr, event_count, halt_requested);
        last_mid_price_ = bar_mid;
        return ok;
    }

    if (config_.latency_model)
    {
        auto latency = config_.latency_model->get_order_latency();
        order.set_earliest_eligible_ts(sim_time + latency);
        pending_orders_.push({acquire_pooled(order_pool_,order), order_seq_++});
        return true;
    }

    if (config_.execution_bar_delay > 0)
    {
        // The symbol-event scheduler is authoritative. The release boundary
        // stamps the actual future observation time before venue submission.
        order.set_earliest_eligible_ts(sim_time);
        if (bar_delayed_orders_.size() == bar_delayed_orders_.capacity())
        {
            constexpr const char* reason =
                "order rejected: delayed-order capacity exhausted";
            order_tracker_.set_status(order.get_order_id(), order_status::rejected);
            auto rej = acquire_pooled(rejection_pool_, order.get_timestamp(),
                order.get_symbol(), order.get_order_id(), reason);
            log_event(*rej);
            publish_event(rej);
            if (!config_.is_threaded()) analytics_.on_event(rej);
            audit_sink_->record_rejection(order, "capacity_reject", reason);
            return true;
        }
        bar_delayed_orders_.push_back({acquire_pooled(order_pool_,order),
                                       order_seq_++,
                                       config_.execution_bar_delay});
        return true;
    }

    order.set_earliest_eligible_ts(sim_time);
    auto order_ptr = acquire_pooled(order_pool_,order);
    if (order.get_tif() == time_in_force::day)
        day_order_ids_.push_back({order.get_symbol(), order.get_order_id()});
    return process_order(order_ptr, event_count, halt_requested);
}

void engine::check_pending_stops(std::string_view event_symbol,
                                 double open, double high, double low,
                                 const std::chrono::system_clock::time_point& sim_time,
                                 std::size_t& event_count, bool& halt_requested)
{
    // last_mid_price_ is the bar close when this runs in the bar loops;
    // restored after each anchored fill so subsequent processing keeps
    // the close reference. Tick callers pass open == high == low ==
    // last_mid_price_, making the anchor a no-op there.
    const double bar_mid = last_mid_price_;

    auto it = pending_stops_.begin();
    while (it != pending_stops_.end() &&
           !halt_requested &&
           !halt_flag_.load(std::memory_order_acquire))
    {
        auto& stop = *it;
        if (stop->get_symbol() != event_symbol)
        {
            ++it;
            continue;
        }
        bool triggered = false;

        if (stop->get_side() == order_side::buy && high >= stop->get_stop_price())
            triggered = true;
        else if (stop->get_side() == order_side::sell && low <= stop->get_stop_price())
            triggered = true;

        if (triggered)
        {
            // Fill reference: the stop price, or the bar open when the
            // bar gapped through the stop. Never the close — that is
            // intra-bar look-ahead and deviates from the convention of
            // filling where the stop was hit.
            const double ref = (stop->get_side() == order_side::buy)
                ? ((open >= stop->get_stop_price()) ? open : stop->get_stop_price())
                : ((open <= stop->get_stop_price()) ? open : stop->get_stop_price());

            last_mid_price_ = ref;
            if (ref != bar_mid)
            {
                // Re-center the synthetic book at the trigger so the
                // converted order walks depth priced around ref, not
                // around the previous close. Skipped for real L2 depth
                // and under the threaded MM preset (the worker owns the
                // book there).
                auto ob = orderbook_registry_.get_or_create(stop->get_symbol());
                if (!mm_worker_ &&
                    !l2_seeded_symbols_.count(stop->get_symbol()))
                {
                    auto mm_trades = market_maker_.replenish(
                        ob, ref, /*update_history=*/false);
                    deliver_mm_book_trades(stop->get_symbol(), mm_trades,
                                           sim_time, event_count, halt_requested);
                }
            }

            if (stop->get_order_type() == order_type::stop)
            {
                auto market_order = acquire_pooled(order_pool_,
                    sim_time, stop->get_symbol(), order_type::market,
                    stop->get_side(), stop->get_quantity(), stop->get_stop_price(),
                    time_in_force::ioc);
                market_order->set_order_id(stop->get_order_id());
                market_order->set_strategy_name(stop->get_strategy_name());
                market_order->set_opener_order_id(stop->get_opener_order_id());
                market_order->set_recv_ns(stop->get_recv_ns());
                market_order->set_earliest_eligible_ts(sim_time);
                if (market_order->get_tif() == time_in_force::day)
                    day_order_ids_.push_back({market_order->get_symbol(), market_order->get_order_id()});
                if (!process_order(market_order, event_count, halt_requested))
                {
                    last_mid_price_ = bar_mid;
                    return;
                }
            }
            else
            {
                auto limit_order = acquire_pooled(order_pool_,
                    sim_time, stop->get_symbol(), order_type::limit,
                    stop->get_side(), stop->get_quantity(), stop->get_price(),
                    stop->get_tif());
                limit_order->set_order_id(stop->get_order_id());
                limit_order->set_strategy_name(stop->get_strategy_name());
                limit_order->set_opener_order_id(stop->get_opener_order_id());
                limit_order->set_recv_ns(stop->get_recv_ns());
                limit_order->set_earliest_eligible_ts(sim_time);
                if (limit_order->get_tif() == time_in_force::day)
                    day_order_ids_.push_back({limit_order->get_symbol(), limit_order->get_order_id()});
                if (!process_order(limit_order, event_count, halt_requested))
                {
                    last_mid_price_ = bar_mid;
                    return;
                }
            }
            last_mid_price_ = bar_mid;
            it = pending_stops_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// notify_position_change_all moved to FillProcessor (Phase 2 engine
// decomposition, 2026-08) — see fills_->notify_position_change_all below and
// core/docs/internal/engine-decomposition.md "Phase 2: Domain Processors".

void engine::register_order_meta(const order_event& o)
{
    const std::uint64_t opener = (o.get_opener_order_id() != 0)
        ? o.get_opener_order_id()
        : o.get_order_id();
    order_meta_[o.get_order_id()] = order_meta{opener, o.get_strategy_name()};
}

std::uint64_t engine::lookup_opener(std::uint64_t order_id) const
{
    auto it = order_meta_.find(order_id);
    return it != order_meta_.end() ? it->second.opener_order_id : 0;
}

const std::string& engine::lookup_strategy_name(std::uint64_t order_id) const
{
    static const std::string empty;
    auto it = order_meta_.find(order_id);
    return it != order_meta_.end() ? it->second.strategy_name : empty;
}

void engine::finalize_strategy_route(IStrategy& strategy,
                                     const std::string& strategy_name,
                                     const order_event& order,
                                     bool halted)
{
    if (halted)
    {
        // Drop pending exit intents; do not arm brackets after terminal halt.
        (void)strategy.take_pending_exit_intents();
        return;
    }

    const uint64_t oid = order.get_order_id();
    if (oid == 0)
    {
        // Paused / never assigned — drain intents and unlock optimistic gates.
        (void)strategy.take_pending_exit_intents();
        fills_->notify_position_change_all(order.get_symbol(),
                                           portfolio_.position_open(order.get_symbol()));
        return;
    }

    const auto st = order_tracker_.get_order_status(oid);
    if (st == order_status::rejected)
    {
        // Venue/risk reject after id assignment — do not arm exits; resync gates.
        (void)strategy.take_pending_exit_intents();
        fills_->notify_position_change_all(order.get_symbol(),
                                           portfolio_.position_open(order.get_symbol()));
        return;
    }

    register_strategy_exit_intent(strategy, strategy_name, order);
}

void engine::register_strategy_exit_intent(IStrategy& strategy,
                                           const std::string& strategy_name,
                                           const order_event& order)
{
    const std::uint64_t order_id = order.get_order_id();
    if (order_id == 0)
    {
        // Opener not assigned (pause/drop) — drain so intents cannot leak
        // and re-arm on a later unrelated entry.
        (void)strategy.take_pending_exit_intents();
        return;
    }
    auto intents = strategy.take_pending_exit_intents();

    // Platform floor / engine_only / union: attach protective SL/TP for any
    // strategy that omitted them. Position-reducing signal closes are skipped
    // so death-cross sells do not arm inverted short brackets.
    double net_qty = 0.0;
    {
        const auto& positions = portfolio_.get_positions();
        auto it = positions.find(order.get_symbol());
        if (it != positions.end())
            net_qty = it->second.qty;
    }
    intents = truetest::exits::apply_default_exit_policy(
        config_.exit_defaults, order, net_qty, std::move(intents));

    for (auto& intent : intents)
    {
        intent.opener_order_id = order_id;
        intent.strategy_name   = strategy_name;
        exit_manager_.register_pending(std::move(intent));
    }
}

bool engine::evaluate_exits(const std::string& symbol, double px,
                            std::chrono::system_clock::time_point ts,
                            std::size_t& event_count,
                            std::int64_t recv_ns)
{
    // See canonical sequence comment in process_order. Closes emitted here
    // go through route_order (which registers meta) + process_order to
    // maintain per-lot / opener discipline and full state propagation.
    auto closes = exit_manager_.on_price(symbol, px, ts);
    if (closes.empty()) return false;
    for (auto& close : closes)
    {
        close.set_recv_ns(recv_ns);
        bool halt = false;
        route_order(close, ts, event_count, halt, /*anchor_immediate=*/true);
        if (halt) return true;
    }
    return false;
}

bool engine::evaluate_exits(const std::string& symbol,
                            double open, double low, double high, double close,
                            std::chrono::system_clock::time_point ts,
                            std::size_t& event_count,
                            std::int64_t recv_ns)
{
    // See canonical sequence comment in process_order. Bar fires go through
    // route_order for consistent meta registration and full propagation,
    // anchored at the fire price computed within the trigger bar.
    auto fires = exit_manager_.on_bar(symbol, open, low, high, close, ts);
    if (fires.empty()) return false;
    for (auto& c : fires)
    {
        c.set_recv_ns(recv_ns);
        bool halt = false;
        route_order(c, ts, event_count, halt, /*anchor_immediate=*/true);
        if (halt) return true;
    }
    return false;
}

double engine::sweep_resting_limits(const std::string& symbol,
                                    double low, double high,
                                    const std::chrono::system_clock::time_point& ts,
                                    std::size_t& event_count, bool& halt_requested,
                                    double bar_volume)
{
    auto it = execution_adapters_.find(symbol);
    if (it == execution_adapters_.end() || !it->second)
        return 0.0;
    // Virtual dispatch (same capability surface as deliver_mm_book_trades).
    if (it->second->sweep_resting_range(symbol, low, high, ts, bar_volume))
    {
        const double consumed = std::clamp(
            it->second->last_sweep_fill_qty(), 0.0,
            std::max(0.0, bar_volume));
        fills_->process_adapter_fills(it->second, event_count, halt_requested);
        return consumed;
    }
    return 0.0;
}
