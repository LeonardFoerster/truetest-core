// Pending-order drain / paper-tape helpers extracted from engine.cpp
// (engine-decomposition: keep freeze surface lean; behavior unchanged).
#include "engine.h"
#include "live_safety_session.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

void engine::clear_pending_state()
{
    pending_stops_.clear();
    while (!pending_orders_.empty()) pending_orders_.pop();
    bar_delayed_orders_.clear();
    bar_delayed_ready_.clear();
    const auto configured = config_.risk.max_open_orders > 0
        ? static_cast<std::size_t>(config_.risk.max_open_orders)
        : DEFAULT_RING_SIZE;

    if (pending_stops_.capacity() < configured)
        pending_stops_.reserve(configured);
    if (pending_orders_capacity_ < configured)
    {
        std::vector<pending_entry> storage;
        storage.reserve(configured);
        pending_orders_ = decltype(pending_orders_){
            &engine::pending_cmp, std::move(storage)};
        pending_orders_capacity_ = configured;
    }
    if (bar_delayed_orders_.capacity() < configured)
        bar_delayed_orders_.reserve(configured);
    if (bar_delayed_ready_.capacity() < bar_delayed_orders_.capacity())
        bar_delayed_ready_.reserve(bar_delayed_orders_.capacity());
    l2_bid_scratch_.clear();
    l2_ask_scratch_.clear();
    if (l2_bid_scratch_.capacity() < kL2SnapshotMaxLevels)
        l2_bid_scratch_.reserve(kL2SnapshotMaxLevels);
    if (l2_ask_scratch_.capacity() < kL2SnapshotMaxLevels)
        l2_ask_scratch_.reserve(kL2SnapshotMaxLevels);
    order_seq_ = 0;
    day_order_ids_.clear();
    if (day_order_ids_.capacity() < configured)
        day_order_ids_.reserve(configured);
}

void engine::prepare_event_logging()
{
    if (config_.event_log_path.empty())
        return;
    if (config_.threading == thread_preset::light)
        throw std::runtime_error(
            "event logging requires inline, standard, full, or extended threading");
    if (config_.threading == thread_preset::inline_mode && !event_logger_)
    {
        if (preopened_event_logger_)
            event_logger_ = std::move(preopened_event_logger_);
        else
            event_logger_ = std::make_unique<EventLogger>(
                config_.event_log_path, config_.compress_log,
                config_.log_max_bytes, config_.log_max_files,
                config_.event_log_reservation);
    }
}

void engine::finalize_inline_event_log() noexcept
{
    if (!event_logger_ || config_.is_threaded())
        return;
    if (event_loop_teardown_compromised_.load(std::memory_order_acquire) ||
        (config_.event_log_reservation &&
         ((!pending_cancels_.empty()) ||
          (durable_log_consumer_ && durable_log_consumer_->compromised()))))
    {
        event_logger_->abandon();
        run_failed_.store(true, std::memory_order_release);
        trigger_halt(
            "incomplete durable inline event log refused finalization");
        return;
    }
    try
    {
        event_logger_->finalize();
    }
    catch (const std::exception& e)
    {
        run_failed_.store(true, std::memory_order_release);
        trigger_halt(e.what());
    }
    catch (...)
    {
        run_failed_.store(true, std::memory_order_release);
        trigger_halt("durable inline event-log finalization failed");
    }
}

double engine::mid_for_symbol(const std::string& symbol) const
{
    {
        std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
        if (auto it = last_mark_prices_.find(symbol);
            it != last_mark_prices_.end()
            && std::isfinite(it->second) && it->second > 0.0)
            return it->second;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

void engine::prepare_mark_prices_for_run(std::size_t symbol_hint)
{
    // Clear + reserve so first-touch mark updates do not rehash under the loop.
    std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
    last_mark_prices_.clear();
    if (symbol_hint > 0)
        last_mark_prices_.reserve(symbol_hint);
}

void engine::drain_pending_orders(
    const std::chrono::system_clock::time_point& sim_time,
    std::size_t& event_count, bool& halt_requested,
    std::string_view event_symbol)
{
    // Preserve the event-loop mid (open/tick of the current symbol). Each
    // pending order may belong to a different symbol; fill mid and MM
    // re-center must track the order's marks, not the event's (EL-MULTISYM-MID).
    const double event_mid = last_mid_price_.load(std::memory_order_relaxed);
    auto submit = [&](const std::shared_ptr<order_event>& order) {
        if (!order)
            return true;
        const auto& sym = order->get_symbol();
        const double mid = mid_for_symbol(sym);
        if (!std::isfinite(mid) || mid <= 0.0)
        {
            trigger_halt("pending order has no valid same-symbol mark");
            halt_requested = true;
            return false;
        }
        last_mid_price_.store(mid, std::memory_order_release);

        if (!mm_worker_ && !l2_seeded_symbols_.count(sym))
        {
            auto ob = orderbook_registry_.get_or_create(sym);
            auto mm_trades = market_maker_.replenish(
                ob, mid, /*update_history=*/false);
            deliver_mm_book_trades(sym, mm_trades, sim_time,
                                   event_count, halt_requested);
            if (halt_requested || halt_flag_.load(std::memory_order_acquire))
                return false;
        }

        if (order->get_tif() == time_in_force::day)
            day_order_ids_.push_back({sym, order->get_order_id()});
        return process_order(order, event_count, halt_requested);
    };

    while (!pending_orders_.empty() &&
           pending_orders_.top().eligible <= sim_time &&
           !halt_requested &&
           !halt_flag_.load(std::memory_order_acquire))
    {
        auto entry = pending_orders_.top();
        pending_orders_.pop();
        if (!submit(entry.order))
        {
            last_mid_price_.store(event_mid, std::memory_order_release);
            return;
        }
    }
    if (halt_requested || halt_flag_.load(std::memory_order_acquire))
    {
        last_mid_price_.store(event_mid, std::memory_order_release);
        return;
    }

    // A non-empty ready buffer means a prior submission stopped on a terminal
    // halt before its remaining due orders could be restored. Never resume or
    // silently discard that retained state.
    if (!bar_delayed_ready_.empty())
    {
        trigger_halt("delayed-order scheduler retained ready orders after halt");
        halt_requested = true;
        last_mid_price_.store(event_mid, std::memory_order_release);
        return;
    }

    const std::size_t delayed_count = bar_delayed_orders_.size();
    if (delayed_count > bar_delayed_ready_.capacity())
    {
        trigger_halt("delayed-order scheduler ready capacity exhausted");
        halt_requested = true;
        last_mid_price_.store(event_mid, std::memory_order_release);
        return;
    }

    // Stable, allocation-free single pass. Survivors retain insertion order in
    // bar_delayed_orders_; due entries retain insertion order in the prewarmed
    // ready buffer. Submission happens only after compaction so an order routed
    // by a submission can use the slot just released and cannot be counted by
    // the same observation.
    std::size_t survivor_count = 0;
    for (std::size_t read = 0; read < delayed_count; ++read)
    {
        auto& entry = bar_delayed_orders_[read];
        const bool same_symbol = entry.order
            && entry.order->get_symbol() == event_symbol;
        if (same_symbol && entry.remaining_symbol_events <= 1)
        {
            bar_delayed_ready_.push_back(std::move(entry));
            continue;
        }

        if (same_symbol)
            --entry.remaining_symbol_events;
        if (survivor_count != read)
            bar_delayed_orders_[survivor_count] = std::move(entry);
        ++survivor_count;
    }
    bar_delayed_orders_.resize(survivor_count);

    auto restore_ready_suffix = [&](std::size_t first_unsubmitted) {
        const std::size_t remaining =
            bar_delayed_ready_.size() - first_unsubmitted;
        if (remaining == 0)
        {
            bar_delayed_ready_.clear();
            return;
        }
        if (remaining > bar_delayed_orders_.capacity()
                            - bar_delayed_orders_.size())
            return;

        const std::size_t old_size = bar_delayed_orders_.size();
        bar_delayed_orders_.resize(old_size + remaining);
        std::size_t left = old_size;
        std::size_t right = bar_delayed_ready_.size();
        std::size_t out = old_size + remaining;
        while (left > 0 && right > first_unsubmitted)
        {
            if (bar_delayed_orders_[left - 1].seq
                > bar_delayed_ready_[right - 1].seq)
            {
                bar_delayed_orders_[--out] =
                    std::move(bar_delayed_orders_[--left]);
            }
            else
            {
                bar_delayed_orders_[--out] =
                    std::move(bar_delayed_ready_[--right]);
            }
        }
        while (right > first_unsubmitted)
        {
            bar_delayed_orders_[--out] =
                std::move(bar_delayed_ready_[--right]);
        }
        bar_delayed_ready_.clear();
    };

    for (std::size_t i = 0; i < bar_delayed_ready_.size(); ++i)
    {
        if (halt_requested || halt_flag_.load(std::memory_order_acquire))
        {
            restore_ready_suffix(i);
            last_mid_price_.store(event_mid, std::memory_order_release);
            return;
        }
        auto order = std::move(bar_delayed_ready_[i].order);
        // Bar-delay fills belong to the observation that released them, not
        // to the signal timestamp plus an artificial nanosecond.
        order->set_earliest_eligible_ts(sim_time);
        if (!submit(order))
        {
            // The failed order matches the old erase-before-submit behavior.
            // Restore every not-yet-submitted due order in global insertion
            // order when bounded capacity permits. This is a linear in-place
            // merge; it allocates nothing. If a pathological re-entrant route
            // consumed those slots, retain the suffix in ready_ for the final
            // fail-closed expiry path instead of growing or dropping it.
            const std::size_t first_unsubmitted = i + 1;
            restore_ready_suffix(first_unsubmitted);
            last_mid_price_.store(event_mid, std::memory_order_release);
            return;
        }
    }
    bar_delayed_ready_.clear();
    last_mid_price_.store(event_mid, std::memory_order_release);
}

void engine::drain_final_pending(std::size_t& event_count, bool& halt_requested)
{
    // No future market observation exists at EOF. Submitting here fabricates
    // both liquidity and a causal timestamp. Expire every never-submitted
    // candidate and release its authoritative lifecycle slot instead.
    (void)halt_requested;
    auto expire = [&](const std::shared_ptr<order_event>& order) {
        if (!order)
            return;
        const auto& sym = order->get_symbol();
        const auto order_id = order->get_order_id();
        order_tracker_.set_status(order_id, order_status::cancelled);
        if (dashboard_builder_) dashboard_builder_->erase_open_order(order_id);
        const auto ts = last_sim_time_.time_since_epoch().count() != 0
            ? last_sim_time_ : order->get_timestamp();
        constexpr const char* reason =
            "backtest_eos_without_future_market_event";
        auto cancel_ev = acquire_pooled(cancel_pool_, ts, sym, order_id, reason);
        log_event(*cancel_ev);
        publish_event(cancel_ev);
        if (!config_.is_threaded())
            analytics_.on_event(cancel_ev);
        audit_sink_->record_cancellation(order_id, sym.c_str(),
            lookup_strategy_name(order_id).c_str(), reason);
        audit_sink_->record_status_transition(order_id,
            order_status::pending, order_status::cancelled, reason);
        ++event_count;
    };

    while (!pending_orders_.empty())
    {
        auto entry = pending_orders_.top();
        pending_orders_.pop();
        expire(entry.order);
    }
    for (const auto& entry : bar_delayed_orders_)
        expire(entry.order);
    bar_delayed_orders_.clear();
    for (const auto& entry : bar_delayed_ready_)
        expire(entry.order);
    bar_delayed_ready_.clear();
    for (const auto& stop : pending_stops_)
        expire(stop);
    pending_stops_.clear();
}

bool engine::discard_unsubmitted_scheduled_orders() noexcept
{
    // Operator stop, terminal failure, and destruction must never force a
    // locally delayed order into the venue. These entries have not reached
    // process_order(), the event log, or an adapter, so teardown only releases
    // their local lifecycle slots and metadata. Any bookkeeping failure makes
    // a reserved shutdown incomplete.
    bool complete = true;
    const auto discard = [&](const std::shared_ptr<order_event>& order) {
        if (!order) return;
        const auto order_id = order->get_order_id();
        try
        {
            order_tracker_.set_status(order_id, order_status::cancelled);
            if (dashboard_builder_)
                dashboard_builder_->erase_open_order(order_id);
            order_meta_.erase(order_id);
        }
        catch (...)
        {
            complete = false;
        }
    };

    while (!pending_orders_.empty())
    {
        auto entry = pending_orders_.top();
        pending_orders_.pop();
        discard(entry.order);
    }
    for (const auto& entry : bar_delayed_orders_)
        discard(entry.order);
    bar_delayed_orders_.clear();
    for (const auto& entry : bar_delayed_ready_)
        discard(entry.order);
    bar_delayed_ready_.clear();
    for (const auto& order : pending_stops_)
        discard(order);
    pending_stops_.clear();
    return complete;
}

void engine::cancel_day_orders()
{
    // Route through the full cancel path so Hybrid/QueueAware paper state is
    // cleared (book-only cancel left QueueAware DAY limits as zombies).
    for (const auto& [symbol, oid] : day_order_ids_)
        cancel_order(symbol, oid, "day_tif_eos");
    day_order_ids_.clear();
    // Latency models only schedule cancels; without a flush, DAY residuals stay
    // in live_quote_count until advance_time. At EOS force-apply pending cancels.
    if (router_ && config_.latency_model)
    {
        const auto flush_ts = std::chrono::system_clock::time_point::max();
        router_->advance_all(flush_ts);
    }
}

void engine::feed_paper_trade_and_drain(const std::string& symbol,
                                        double price, double qty,
                                        std::chrono::system_clock::time_point ts,
                                        std::size_t& event_count, bool& halt_requested)
{
    // QueueAware limits only advance on on_trade. Without a paper tape they
    // never fill in pure backtest — feed a synthetic print when maker-queue
    // paper mode is active (same lossy bar approximation as shadow bar path).
    // Find-only: never create Hybrid/QueueAware just because a bar/tick arrived
    // with no resting strategy orders (matches deliver_mm_book_trades).
    if (!config_.maker_queue_model)
        return;
    if (!(price > 0.0) || !(qty > 0.0))
        return;
    if (halt_flag_.load(std::memory_order_acquire))
        return;
    auto it = execution_adapters_.find(symbol);
    if (it == execution_adapters_.end() || !it->second)
        return;
    it->second->on_trade(symbol, price, qty, ts);
    process_adapter_fills(it->second, event_count, halt_requested);
}

void engine::add_strategy(std::shared_ptr<IStrategy> strategy, const std::string& name)
{
    if (!strategy) return;
    // Empty name is reserved: dispatch_fill_to_strategy() treats an empty
    // resolved strategy_name as "route to primary" (FR-08 fallback for
    // callers that omit set_primary_strategy_name). An additional strategy
    // registered with "" would collide with that fallback and silently
    // steal fills that belong to it instead of the primary.
    if (name.empty())
    {
        std::cerr << "  ! add_strategy: empty name not allowed for additional "
                      "strategies (reserved for primary fallback), ignoring\n";
        return;
    }
    additional_strategies_.push_back(std::move(strategy));
    additional_strategy_names_.push_back(name);
}

void engine::setup_event_loop_infra()
{
#ifdef HAS_QUESTDB
    questdb_begin();
#endif
    start_workers();
    pin_event_loop_thread();
}

void engine::teardown_event_loop_infra()
{
    // Drain before stopping workers so that any last events processed
    // by workers have their releases staged and reclaimed before rings
    // and pools become unreachable.
    drain_object_pool_returns();

    stop_workers();
    // Provider shutdown can enqueue one final settlement.  Finalize the
    // synchronous ledger only after stop_workers() has quiesced the producer
    // and drained that settlement.
    finalize_inline_event_log();
#ifdef HAS_QUESTDB
    questdb_end();
#endif
}

void engine::fail_event_loop_infra(std::string_view reason) noexcept
{
    // This path runs during exception unwinding or after DataBridge has
    // converted a callback/transport exception into runtime_failure. Latch
    // every failure authority before cleanup so neither a clean-looking
    // worker drain nor a later retry can turn a partial run into an
    // authoritative ledger or re-arm venue callbacks.
    event_loop_teardown_compromised_.store(true, std::memory_order_release);
    worker_failed_.store(true, std::memory_order_release);
    if (durable_log_consumer_)
        durable_log_consumer_->compromise();
    trigger_halt(reason);

    try
    {
        teardown_event_loop_infra();
    }
    catch (...)
    {
        // stop_workers() normally owns this sequence.  Its provider-facing
        // virtual calls can throw, so retain a no-throw last resort that still
        // disarms callbacks and joins every worker that was successfully
        // launched before startup failed.
        provider_callbacks_armed_.store(false, std::memory_order_release);
        if (callbacks_armed_flag_)
            callbacks_armed_flag_->store(false, std::memory_order_release);

        try
        {
            if (worker_watchdog_) worker_watchdog_->stop();
        }
        catch (...) {}

        Worker* workers[] = {
            observer_worker_.get(), logging_worker_.get(), risk_worker_.get(),
            stats_worker_.get(), risk_stats_worker_.get(), mm_worker_.get()};
        for (auto* worker : workers)
            if (worker) worker->stop();

        for (auto& thread : worker_threads_)
        {
            if (!thread.joinable()) continue;
            try { thread.join(); }
            catch (...) { std::terminate(); }
        }
        worker_threads_.clear();

        try
        {
            (void)finalize_live_shutdown(live_shutdown_reason::engine_halt);
        }
        catch (...) {}
        try { revoke_provider_callbacks(); }
        catch (...) {}

#ifdef HAS_QUESTDB
        try { questdb_end(); }
        catch (...) {}
#endif
    }

    // Covers inline logging and a preopened reserved logger not yet moved into
    // LoggingWorker (for example, failure before worker construction).
    if (logging_worker_) logging_worker_->abandon();
    if (event_logger_) event_logger_->abandon();
    if (preopened_event_logger_) preopened_event_logger_->abandon();
}

void engine::report_run_summary(std::size_t event_count,
                                std::chrono::high_resolution_clock::time_point start_time)
{
    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start_time).count();

    if (config_.show_progress) {
        std::cout << std::endl;
        std::cout << "Trades executed: " << portfolio_.get_total_trades()
                  << " in " << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

        double throughput = static_cast<double>(event_count) / (elapsed_ms / 1000.0);
        std::cout << "Event throughput: " << throughput << " events/second" << std::endl;
    }
}

void engine::fold_research_counters_into_export_analytics()
{
    // Soft post-fill is counted on the engine event loop (analytics_ + local
    // counter). Threaded get_analytics() returns a worker Analytics that never
    // saw those counters — fold after join so export/report is honest.
    const std::size_t soft = soft_post_fill_breaches_;
    const std::size_t rejects = data_rows_rejected_ > 0
        ? data_rows_rejected_
        : analytics_.data_rows_rejected();
    analytics_.fold_research_counters(soft, rejects);
    switch (config_.threading)
    {
    case thread_preset::light:
        if (observer_worker_)
            observer_worker_->analytics().fold_research_counters(soft, rejects);
        break;
    case thread_preset::standard:
        if (risk_stats_worker_)
            risk_stats_worker_->analytics().fold_research_counters(soft, rejects);
        break;
    case thread_preset::full:
    case thread_preset::extended:
        if (stats_worker_)
            stats_worker_->analytics().fold_research_counters(soft, rejects);
        break;
    default:
        break;
    }
}

std::size_t engine::total_live_quotes() const
{
    std::size_t total = 0;
    for (const auto& [_, ad] : execution_adapters_)
    {
        if (ad)
            total += ad->live_quote_count();
    }
    if (config_.provider)
    {
        if (auto pa = config_.provider->get_execution_adapter())
            total += pa->live_quote_count();
    }
    return total;
}
