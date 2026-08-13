// Pending-order drain / paper-tape helpers extracted from engine.cpp
// (engine-decomposition: keep freeze surface lean; behavior unchanged).
#include "engine.h"

#include <iostream>
#include <utility>

void engine::clear_pending_state()
{
    pending_stops_.clear();
    while (!pending_orders_.empty()) pending_orders_.pop();
    order_seq_ = 0;
    day_order_ids_.clear();
}

double engine::mid_for_symbol(const std::string& symbol) const
{
    {
        std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
        if (auto it = last_mark_prices_.find(symbol);
            it != last_mark_prices_.end() && it->second > 0.0)
            return it->second;
    }
    return last_mid_price_.load(std::memory_order_relaxed);
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
    bool force_all)
{
    // Preserve the event-loop mid (open/tick of the current symbol). Each
    // pending order may belong to a different symbol; fill mid and MM
    // re-center must track the order's marks, not the event's (EL-MULTISYM-MID).
    // force_all (EOS): ignore eligibility cutoff; MM trades use order ts.
    const double event_mid = last_mid_price_.load(std::memory_order_relaxed);
    while (!pending_orders_.empty() &&
           (force_all ||
            pending_orders_.top().order->get_earliest_eligible_ts() <= sim_time) &&
           !halt_requested &&
           !halt_flag_.load(std::memory_order_acquire))
    {
        auto entry = pending_orders_.top();
        pending_orders_.pop();
        const auto& sym = entry.order->get_symbol();
        const double mid = mid_for_symbol(sym);
        last_mid_price_.store(mid, std::memory_order_release);

        if (!mm_worker_ && !l2_seeded_symbols_.count(sym))
        {
            auto ob = orderbook_registry_.get_or_create(sym);
            auto mm_trades = market_maker_.replenish(
                ob, mid, /*update_history=*/false);
            const auto mm_ts = force_all
                ? entry.order->get_earliest_eligible_ts()
                : sim_time;
            deliver_mm_book_trades(sym, mm_trades, mm_ts,
                                   event_count, halt_requested);
            if (halt_requested || halt_flag_.load(std::memory_order_acquire))
            {
                last_mid_price_.store(event_mid, std::memory_order_release);
                return;
            }
        }

        if (entry.order->get_tif() == time_in_force::day)
            day_order_ids_.push_back({sym, entry.order->get_order_id()});
        if (!process_order(entry.order, event_count, halt_requested))
        {
            last_mid_price_.store(event_mid, std::memory_order_release);
            return;
        }
    }
    last_mid_price_.store(event_mid, std::memory_order_release);
}

void engine::drain_final_pending(std::size_t& event_count, bool& halt_requested)
{
    // Honor both the local out-param and the process-wide halt_flag_ so a
    // risk halt mid-drain does not keep submitting remaining pendings.
    drain_pending_orders(/*sim_time=*/{}, event_count, halt_requested,
                         /*force_all=*/true);
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

    if (event_logger_) event_logger_->flush();
    stop_workers();
#ifdef HAS_QUESTDB
    questdb_end();
#endif
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
