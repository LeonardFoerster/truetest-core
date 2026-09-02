// Pending-order drain / paper-tape helpers extracted from engine.cpp
// (engine-decomposition: keep freeze surface lean; behavior unchanged).
#include "engine.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

void engine::clear_pending_state()
{
    // pending_stops_ is owned by OrderIntentProcessor as of Phase 3 — clear
    // via its own narrow reset hook rather than reaching into a member this
    // class no longer has. Latency queue, bar-delay buffers, seq counter,
    // and day-order list are canonically owned by pending_scheduler_
    // (PendingOrderScheduler). L2 scratch buffers below are market-domain,
    // unrelated to either.
    orders_->clear_pending_stops();
    pending_scheduler_->clear();
    const auto configured = config_.risk.max_open_orders > 0
        ? static_cast<std::size_t>(config_.risk.max_open_orders)
        : DEFAULT_RING_SIZE;
    pending_scheduler_->reserve_bar_delay_capacity(configured);
    // R3: size the authoritative ledger once per run so no order-lifecycle
    // transition rehashes under the event loop.
    order_tracker_.reserve(configured * 4);
    l2_bid_scratch_.clear();
    l2_ask_scratch_.clear();
    if (l2_bid_scratch_.capacity() < kL2SnapshotMaxLevels)
        l2_bid_scratch_.reserve(kL2SnapshotMaxLevels);
    if (l2_ask_scratch_.capacity() < kL2SnapshotMaxLevels)
        l2_ask_scratch_.reserve(kL2SnapshotMaxLevels);
}

void engine::prepare_event_logging()
{
    if (config_.event_log_path.empty())
        return;
    if (config_.threading == thread_preset::light)
        throw std::runtime_error(
            "event logging requires inline, standard, full, or extended threading");
    if (config_.threading == thread_preset::inline_mode && !event_logger_)
        event_logger_ = std::make_unique<EventLogger>(
            config_.event_log_path, config_.compress_log,
            config_.log_max_bytes, config_.log_max_files);
}

void engine::finalize_inline_event_log() noexcept
{
    if (!event_logger_ || config_.is_threaded())
        return;
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

// mid_for_symbol moved to OrderIntentProcessor::mid_for_symbol (Phase 2) —
// its only caller (drain_pending_orders, below) moved with it.

void engine::prepare_mark_prices_for_run(std::size_t symbol_hint)
{
    // Clear + reserve so first-touch mark updates do not rehash under the loop.
    std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
    last_mark_prices_.clear();
    if (symbol_hint > 0)
        last_mark_prices_.reserve(symbol_hint);
}

// drain_pending_orders moved to OrderIntentProcessor::drain_due (Phase 2) —
// see order_intent_processor.cpp. Call sites now say
// orders_->drain_due(...).

// drain_final_pending + cancel_day_orders moved to OrderIntentProcessor
// (Phase 3), consolidated into finalize_end_of_stream (both were always
// called back-to-back at all 5 original call sites — see
// order_intent_processor.cpp). Call sites now say
// orders_->finalize_end_of_stream(...).

void engine::feed_paper_trade_and_drain(const std::string& symbol,
                                        double price, double qty,
                                        std::optional<order_side> aggressor_side,
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
    it->second->on_trade(symbol, price, qty, aggressor_side, ts);
    fills_->process_adapter_fills(it->second, event_count, halt_requested);
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

void engine::infer_bar_interval()
{
    // F-08: a bar's timestamp is its open. The decision a strategy makes from
    // that bar happens at its close, one interval later. The interval is a
    // property of the data, not of the future: it is read from gaps between
    // bars already loaded, and a series that does not have a single dominant
    // gap leaves it at zero (decision time == bar open, the pre-F-08
    // behaviour) rather than inventing one.
    bar_interval_ = std::chrono::system_clock::duration::zero();
    if (!data_handler_) return;

    const std::size_t n = data_handler_->bar_count();
    if (n < 3) return;

    // Modal positive gap over a bounded prefix — robust to a symbol change or
    // a session break without walking a multi-million-bar series.
    constexpr std::size_t kProbe = 256;
    const std::size_t limit = std::min(n, kProbe);
    std::map<std::chrono::system_clock::duration::rep, std::size_t> gaps;
    std::size_t samples = 0;
    for (std::size_t i = 1; i < limit; ++i)
    {
        const auto a = data_handler_->bar_at(i - 1).ts;
        const auto b = data_handler_->bar_at(i).ts;
        if (a.time_since_epoch().count() == 0 || b.time_since_epoch().count() == 0)
            continue;
        if (data_handler_->bar_symbol_at(i) != data_handler_->bar_symbol_at(i - 1))
            continue;
        const auto gap = (b - a).count();
        if (gap <= 0) continue;
        ++gaps[gap];
        ++samples;
    }
    if (samples < 2) return;

    auto best = gaps.begin();
    for (auto it = gaps.begin(); it != gaps.end(); ++it)
        if (it->second > best->second) best = it;

    // Require a clear majority; an irregular series has no honest interval.
    if (best->second * 2 <= samples) return;
    bar_interval_ = std::chrono::system_clock::duration(best->first);
}

void engine::validate_instrument_overrides()

{
    // F-07a: a configured --instrument spec that matches no symbol in the
    // loaded series does nothing at all — tick size, lot size, min quantity
    // and min notional are inert and the run is byte-identical to one
    // without the flag. That is indistinguishable from "the filters passed",
    // which is exactly how the audit's proof run looked. Fail closed instead.
    if (!instrument_spec_cache_) return;
    if (config_.instrument_overrides.empty()) return;
    if (!data_handler_) return;

    std::unordered_set<std::string> present;
    for (std::size_t i = 0; i < data_handler_->bar_count(); ++i)
        present.insert(data_handler_->bar_symbol_at(i));
    for (std::size_t i = 0; i < data_handler_->tick_count(); ++i)
        present.insert(data_handler_->tick_at(i).symbol);

    const auto missing = instrument_spec_cache_->unmatched_overrides(present);
    if (missing.empty()) return;

    std::string msg =
        "instrument spec(s) match no symbol in the loaded data, so their "
        "tick/lot/min-qty/min-notional filters would silently do nothing: ";
    for (std::size_t i = 0; i < missing.size(); ++i)
    {
        if (i) msg += ", ";
        msg += missing[i];
    }
    msg += ". Symbols present: ";
    if (present.empty())
    {
        msg += "(none)";
    }
    else
    {
        std::vector<std::string> sorted(present.begin(), present.end());
        std::sort(sorted.begin(), sorted.end());
        for (std::size_t i = 0; i < sorted.size(); ++i)
        {
            if (i) msg += ", ";
            msg += sorted[i].empty() ? "\"\" (unbound — pass --symbol)" : sorted[i];
        }
    }
    throw std::runtime_error(msg);
}

void engine::setup_event_loop_infra()
{
    validate_instrument_overrides();
    infer_bar_interval();

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

        const double safe_elapsed_s = (elapsed_ms > 0 ? static_cast<double>(elapsed_ms) : 1.0) / 1000.0;
        double throughput = static_cast<double>(event_count) / safe_elapsed_s;
        std::cout << "Event throughput: " << throughput << " events/second" << std::endl;
    }
}

void engine::fold_research_counters_into_export_analytics()
{
    // Soft post-fill is counted on the engine event loop (analytics_ + a
    // counter now owned by FillProcessor, its sole writer). Threaded
    // get_analytics() returns a worker Analytics that never saw those
    // counters — fold after join so export/report is honest.
    const std::size_t soft = fills_->soft_post_fill_breach_count();
    const std::size_t rejects = data_rows_rejected_ > 0
        ? data_rows_rejected_
        : analytics_.data_rows_rejected();

    // F-05a / F-06: same fold-after-join reason as the research counters —
    // the bankruptcy latch lives on portfolio_ and the intent lifecycle on
    // exit_manager_, neither of which a worker Analytics ever observes.
    const bool bankrupt = portfolio_.is_bankrupt();
    const double bankrupt_equity = portfolio_.bankrupt_equity();
    const auto& ec = exit_manager_.counters();
    const Analytics::exit_lifecycle_counts exits{
        ec.pending_registered, ec.armed, ec.cancelled,
        ec.pending_evicted, ec.slippage_disarms, ec.flatten_requests};

    auto fold = [&](Analytics& a) {
        a.fold_research_counters(soft, rejects);
        a.fold_lifecycle_counters(bankrupt, bankrupt_equity, exits);
    };

    fold(analytics_);
    switch (config_.threading)
    {
    case thread_preset::light:
        if (observer_worker_) fold(observer_worker_->analytics());
        break;
    case thread_preset::standard:
        if (risk_stats_worker_) fold(risk_stats_worker_->analytics());
        break;
    case thread_preset::full:
    case thread_preset::extended:
        if (stats_worker_) fold(stats_worker_->analytics());
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
