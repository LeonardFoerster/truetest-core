#include "engine.h"
#include "../data/data_handler.h"
#include "../execution/portfolio.h"
#include "../execution/fee_model.h"
#include "../execution/latency_model.h"
#include "../orderbook/fill_model.h"

#include <iostream>
#include <vector>
#include <queue>
#include <chrono>
#include <iomanip>
#include <algorithm>

engine::engine(std::shared_ptr<data_handler> dh,
               std::shared_ptr<orderbook> ob,
               std::shared_ptr<IStrategy> strategy,
               engine_config config)
    : config_(std::move(config)), data_handler_(std::move(dh)), strategy_(std::move(strategy)),
      risk_manager_(config_.risk),
      market_maker_(config_.seed != 0 ? MarketMaker(static_cast<unsigned>(config_.seed + 1))
                                      : MarketMaker())
{
    // Register the provided orderbook under a default symbol
    // The actual symbol will be determined at runtime from market data
    if (ob)
    {
        // We'll store it and assign to the first symbol we see
        // For now, pre-register it — the run() loop will use get_adapter() per symbol
        orderbook_registry_ = OrderbookRegistry();
    }

    // Rings are created in start_workers() based on the active preset
}

std::shared_ptr<IExecutionAdapter> engine::get_adapter(const std::string& symbol)
{
    auto it = execution_adapters_.find(symbol);
    if (it != execution_adapters_.end())
        return it->second;

    auto ob = orderbook_registry_.get_or_create(symbol);

    std::shared_ptr<IExecutionAdapter> adapter;
    if (config_.mode == engine_mode::live)
        adapter = std::make_shared<ExchangeAdapter>();
    else
        adapter = std::make_shared<LocalBookAdapter>(
            ob, config_.fee_model, config_.fill_model,
            config_.seed != 0 ? static_cast<unsigned>(config_.seed + 2) : 42u);

    execution_adapters_[symbol] = adapter;
    return adapter;
}

void engine::log_event(const event& ev)
{
    // When threaded, logging happens in the logging/observer worker
    if (config_.is_threaded())
        return;
    if (event_logger_)
        event_logger_->log(ev);
}

void engine::publish_event(const event_pointer& ev)
{
    switch (config_.threading)
    {
    case thread_preset::inline_mode:
        return;

    case thread_preset::light:
        if (observer_ring_ && !observer_ring_->try_push(ev)) observer_drops_++;
        return;

    case thread_preset::standard:
        if (logging_ring_ && !logging_ring_->try_push(ev)) logging_drops_++;
        if (risk_stats_ring_ && !risk_stats_ring_->try_push(ev)) risk_stats_drops_++;
        return;

    case thread_preset::full:
        if (logging_ring_ && !logging_ring_->try_push(ev)) logging_drops_++;
        if (risk_ring_ && !risk_ring_->try_push(ev)) risk_drops_++;
        if (stats_ring_ && !stats_ring_->try_push(ev)) stats_drops_++;
        return;

    case thread_preset::extended:
        if (logging_ring_ && !logging_ring_->try_push(ev)) logging_drops_++;
        if (risk_ring_ && !risk_ring_->try_push(ev)) risk_drops_++;
        if (stats_ring_ && !stats_ring_->try_push(ev)) stats_drops_++;
        if (mm_ring_ && !mm_ring_->try_push(ev)) mm_drops_++;
        return;
    }
}

std::unique_ptr<LoggingWorker> engine::make_logging_worker()
{
    auto text_sink = LoggingWorker::log_sink::none;
    if (config_.log_to_stdout)
        text_sink = LoggingWorker::log_sink::stdout_sink;
    else if (!config_.text_log_path.empty())
        text_sink = LoggingWorker::log_sink::file_sink;

    return std::make_unique<LoggingWorker>(
        config_.event_log_path, text_sink, config_.text_log_path);
}

void engine::start_workers()
{
    if (!config_.is_threaded())
        return;

    halt_flag_.store(false, std::memory_order_release);
    worker_failed_.store(false, std::memory_order_release);

    // Helper to wire shared failure flag to any worker
    auto wire_failure = [this](Worker& w) { w.set_failure_flag(worker_failed_); };

    // Build core map for pinning
    auto core_map = build_core_map();

    auto find_core = [&](core_role role) -> int {
        if (config_.disable_pinning) return -1;
        // Check explicit overrides first
        switch (role) {
        case core_role::event_loop: if (config_.pin_event_loop >= 0) return config_.pin_event_loop; break;
        case core_role::logging:    if (config_.pin_logging >= 0)    return config_.pin_logging; break;
        case core_role::risk:       if (config_.pin_risk >= 0)       return config_.pin_risk; break;
        case core_role::stats:      if (config_.pin_stats >= 0)      return config_.pin_stats; break;
        case core_role::market_maker: if (config_.pin_mm >= 0)       return config_.pin_mm; break;
        }
        // Fall back to auto-detected core map
        for (const auto& ca : core_map)
            if (ca.role == role) return ca.core_id;
        return -1;
    };

    switch (config_.threading)
    {
    case thread_preset::inline_mode:
        return;

    case thread_preset::light:
    {
        observer_ring_ = std::make_shared<EventRing>();
        observer_worker_ = std::make_unique<ObserverWorker>(risk_manager_, halt_flag_);
        wire_failure(*observer_worker_);

        worker_threads_.emplace_back([this]() {
            observer_worker_->run(*observer_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::logging));
        break;
    }

    case thread_preset::standard:
    {
        logging_ring_ = std::make_shared<EventRing>();
        risk_stats_ring_ = std::make_shared<EventRing>();

        logging_worker_ = make_logging_worker();
        risk_stats_worker_ = std::make_unique<RiskStatsWorker>(risk_manager_, halt_flag_);
        wire_failure(*logging_worker_);
        wire_failure(*risk_stats_worker_);

        worker_threads_.emplace_back([this]() {
            logging_worker_->run(*logging_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::logging));

        worker_threads_.emplace_back([this]() {
            risk_stats_worker_->run(*risk_stats_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::risk));
        break;
    }

    case thread_preset::full:
    {
        logging_ring_ = std::make_shared<EventRing>();
        risk_ring_ = std::make_shared<EventRing>();
        stats_ring_ = std::make_shared<EventRing>();

        logging_worker_ = make_logging_worker();
        risk_worker_ = std::make_unique<RiskWorker>(risk_manager_, halt_flag_);
        stats_worker_ = std::make_unique<StatsWorker>();
        wire_failure(*logging_worker_);
        wire_failure(*risk_worker_);
        wire_failure(*stats_worker_);

        worker_threads_.emplace_back([this]() {
            logging_worker_->run(*logging_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::logging));

        worker_threads_.emplace_back([this]() {
            risk_worker_->run(*risk_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::risk));

        worker_threads_.emplace_back([this]() {
            stats_worker_->run(*stats_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::stats));
        break;
    }

    case thread_preset::extended:
    {
        // Full + market maker worker
        logging_ring_ = std::make_shared<EventRing>();
        risk_ring_ = std::make_shared<EventRing>();
        stats_ring_ = std::make_shared<EventRing>();
        mm_ring_ = std::make_shared<EventRing>();
        mm_order_ring_ = std::make_shared<MMRing>();

        logging_worker_ = make_logging_worker();
        risk_worker_ = std::make_unique<RiskWorker>(risk_manager_, halt_flag_);
        stats_worker_ = std::make_unique<StatsWorker>();
        mm_worker_ = std::make_unique<MarketMakerWorker>(
            config_.seed != 0 ? static_cast<unsigned>(config_.seed + 3) : 42u,
            *mm_order_ring_);
        wire_failure(*logging_worker_);
        wire_failure(*risk_worker_);
        wire_failure(*stats_worker_);
        wire_failure(*mm_worker_);

        worker_threads_.emplace_back([this]() {
            logging_worker_->run(*logging_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::logging));

        worker_threads_.emplace_back([this]() {
            risk_worker_->run(*risk_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::risk));

        worker_threads_.emplace_back([this]() {
            stats_worker_->run(*stats_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::stats));

        worker_threads_.emplace_back([this]() {
            mm_worker_->run(*mm_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::market_maker));
        break;
    }
    }
}

void engine::stop_workers()
{
    if (!config_.is_threaded())
        return;

    // Signal all active workers to stop
    if (observer_worker_) observer_worker_->stop();
    if (logging_worker_) logging_worker_->stop();
    if (risk_worker_) risk_worker_->stop();
    if (stats_worker_) stats_worker_->stop();
    if (risk_stats_worker_) risk_stats_worker_->stop();
    if (mm_worker_) mm_worker_->stop();

    // Join all threads
    for (auto& t : worker_threads_)
    {
        if (t.joinable())
            t.join();
    }
    worker_threads_.clear();

    // Drain any remaining events from MM inbound ring to avoid
    // use-after-free when the ring outlives the worker's object pool.
    if (mm_order_ring_)
    {
        event_pointer ev;
        while (mm_order_ring_->try_pop(ev)) {}
    }

    // Report worker exceptions
    Worker* all_workers[] = {
        logging_worker_.get(), risk_worker_.get(), stats_worker_.get(),
        observer_worker_.get(), risk_stats_worker_.get(), mm_worker_.get()
    };
    for (auto* w : all_workers)
    {
        if (w && w->has_failed())
        {
            try { std::rethrow_exception(w->get_exception()); }
            catch (const std::exception& e) {
                std::cerr << "  Worker failed: " << e.what() << "\n";
            }
            catch (...) {
                std::cerr << "  Worker failed with unknown exception\n";
            }
        }
    }

    // Report drop counts
    std::size_t total_drops = logging_drops_ + risk_drops_ + stats_drops_
                            + observer_drops_ + risk_stats_drops_ + mm_drops_;
    if (total_drops > 0)
    {
        std::cerr << "  WARNING: " << total_drops << " events dropped from ring buffers.\n";
        std::cerr << "  Drops: logging=" << logging_drops_
                  << " risk=" << risk_drops_
                  << " stats=" << stats_drops_
                  << " observer=" << observer_drops_
                  << " risk_stats=" << risk_stats_drops_
                  << " mm=" << mm_drops_ << "\n";
    }
}

void engine::print_summary()
{
    switch (config_.threading)
    {
    case thread_preset::inline_mode:
        analytics_.print_report();
        return;
    case thread_preset::light:
        if (observer_worker_)
            observer_worker_->analytics().print_report();
        return;
    case thread_preset::standard:
        if (risk_stats_worker_)
            risk_stats_worker_->analytics().print_report();
        return;
    case thread_preset::full:
    case thread_preset::extended:
        if (stats_worker_)
            stats_worker_->analytics().print_report();
        return;
    }
}

void engine::run()
{
    if (!data_handler_) throw std::runtime_error("missing dependencies");

    // Dispatch to tick-based loop if tick data is available
    if (data_handler_->has_tick_data())
    {
        run_tick_data();
        return;
    }

    if (!data_handler_->has_bar_data()) {
        throw std::runtime_error("no data loaded — call IDataSource::load_data() before run()");
    }

    if (!config_.event_log_path.empty())
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path);

    start_workers();

    // Use a fixed epoch when seed is set for deterministic replay
    const auto base_ts = (config_.seed != 0)
        ? std::chrono::system_clock::time_point(std::chrono::milliseconds(0))
        : std::chrono::system_clock::now();
    const auto n = data_handler_->db_data_symbol.size();
    const auto start = std::chrono::high_resolution_clock::now();
    std::cout << "\rProgress: 0.000% | Trades executed: 0" << std::flush;

    std::size_t event_count = 0;
    auto last_report_time = std::chrono::steady_clock::now();

    // Pending stop orders: triggered when market price crosses stop_price
    std::vector<std::shared_ptr<order_event>> pending_stops;

    // Pending orders min-heap: sorted by (eligible time, sequence number for FIFO)
    struct pending_entry
    {
        std::shared_ptr<order_event> order;
        uint64_t seq;
    };

    auto pending_cmp = [](const pending_entry& a, const pending_entry& b)
    {
        if (a.order->get_earliest_eligible_ts() != b.order->get_earliest_eligible_ts())
            return a.order->get_earliest_eligible_ts() > b.order->get_earliest_eligible_ts();
        return a.seq > b.seq;
    };

    std::priority_queue<pending_entry, std::vector<pending_entry>, decltype(pending_cmp)> pending_orders(pending_cmp);
    uint64_t order_seq = 0;

    // Track day orders for session-end cancellation: (symbol, order_id)
    std::vector<std::pair<std::string, uint64_t>> day_order_ids;

    bool halt_requested = false;

    // Submits an order via the per-symbol execution adapter and processes resulting fills
    auto process_order = [&](const std::shared_ptr<order_event>& o) -> bool
    {
        // Pre-order risk check (inline only — when threaded, RiskWorker handles this)
        if (!config_.is_threaded())
        {
            auto snap = analytics_.snapshot();
            auto action = risk_manager_.check_order(*o, portfolio_, snap);
            if (action == risk_action::halt)
            {
                halt_requested = true;
                return false;
            }
            if (action == risk_action::reject)
                return true; // drop order, continue engine
        }

        // Track day orders for end-of-session cancellation
        if (o->get_tif() == time_in_force::day)
            day_order_ids.push_back({o->get_symbol(), o->get_order_id()});
        log_event(*o);
        publish_event(o);
        if (!config_.is_threaded())
            analytics_.on_event(o);

        auto adapter = get_adapter(o->get_symbol());

        // Update mid price on the adapter (for fill model distance calculations)
        if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
            local->set_mid_price(last_mid_price_);

        adapter->submit_order(*o);

        std::vector<fill_event> fills;
        if (adapter->poll_fills(fills))
        {
            for (auto& f : fills)
            {
                auto fill_ptr = fill_pool_.acquire(f);
                log_event(f);
                portfolio_.on_fill(f);
                strategy_->set_position_open(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                publish_event(fill_ptr);
                if (!config_.is_threaded())
                    analytics_.on_event(fill_ptr);
                event_count++;

                // Post-fill risk check (inline only — when threaded, RiskWorker handles this)
                if (!config_.is_threaded())
                {
                    auto post_snap = analytics_.generate_report();
                    auto post_action = risk_manager_.check_post_fill(f, portfolio_, post_snap);
                    if (post_action == risk_action::halt)
                    {
                        halt_requested = true;
                        return false;
                    }
                }
            }
        }
        event_count++;
        return true;
    };

    for (std::size_t i = 0; i < n && !halt_requested
             && !halt_flag_.load(std::memory_order_acquire)
             && !worker_failed_.load(std::memory_order_acquire); ++i)
    {
        market_event mkt(
            base_ts + std::chrono::milliseconds(static_cast<long long>(i)),
            data_handler_->db_data_symbol[i],
            data_handler_->db_data_open_value[i],
            data_handler_->db_data_high_value[i],
            data_handler_->db_data_low_value[i],
            data_handler_->db_data_close_value[i],
            data_handler_->db_data_volume_value[i]
        );

        auto sim_time = mkt.get_timestamp();
        const auto& symbol = mkt.get_symbol();

        // Drain eligible pending orders before processing this market event
        while (!pending_orders.empty() &&
               pending_orders.top().order->get_earliest_eligible_ts() <= sim_time)
        {
            auto entry = pending_orders.top();
            pending_orders.pop();
            if (!process_order(entry.order)) break;
        }
        if (halt_requested) break;

        // Track mid price for fill model distance calculations
        last_mid_price_ = mkt.get_close();

        // Check pending stop orders for triggers
        {
            auto it = pending_stops.begin();
            while (it != pending_stops.end())
            {
                auto& stop = *it;
                bool triggered = false;

                if (stop->get_side() == order_side::buy && mkt.get_high() >= stop->get_stop_price())
                    triggered = true;
                else if (stop->get_side() == order_side::sell && mkt.get_low() <= stop->get_stop_price())
                    triggered = true;

                if (triggered)
                {
                    if (stop->get_order_type() == order_type::stop)
                    {
                        // Convert to market order
                        auto market_order = order_pool_.acquire(
                            sim_time, stop->get_symbol(), order_type::market,
                            stop->get_side(), stop->get_quantity(), stop->get_stop_price(),
                            time_in_force::ioc);
                        market_order->set_order_id(stop->get_order_id());
                        market_order->set_earliest_eligible_ts(sim_time);
                        if (!process_order(market_order)) break;
                    }
                    else // stop_limit
                    {
                        // Convert to limit order at the limit price
                        auto limit_order = order_pool_.acquire(
                            sim_time, stop->get_symbol(), order_type::limit,
                            stop->get_side(), stop->get_quantity(), stop->get_price(),
                            stop->get_tif());
                        limit_order->set_order_id(stop->get_order_id());
                        limit_order->set_earliest_eligible_ts(sim_time);
                        if (!process_order(limit_order)) break;
                    }
                    it = pending_stops.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // Reactive market maker: replenish depleted book levels per symbol
        auto ob = orderbook_registry_.get_or_create(symbol);
        if (!preset_has_mm_worker(config_.threading))
            market_maker_.replenish(ob, last_mid_price_);

        // Drain market maker inbound orders (extended preset only)
        if (mm_order_ring_)
        {
            event_pointer mm_ev;
            while (mm_order_ring_->try_pop(mm_ev))
            {
                if (mm_ev->get_type() == event_type::order)
                {
                    auto& mm_order = static_cast<order_event&>(*mm_ev);
                    auto mm_ob = orderbook_registry_.get_or_create(mm_order.get_symbol());
                    auto mm_side = (mm_order.get_side() == order_side::buy) ? side::buy : side::sell;
                    auto mm_ob_order = std::make_shared<order>(
                        ob_order_type::good_till_cancel, mm_order.get_order_id(),
                        mm_side, Price::from_double(mm_order.get_price()),
                        static_cast<quantity>(mm_order.get_quantity()));
                    mm_ob->add_order(mm_ob_order);
                }
            }
        }

        // Process market event
        auto mkt_ptr = market_pool_.acquire(mkt);
        auto order_opt = strategy_->on_market(mkt);
        log_event(mkt);
        publish_event(mkt_ptr);
        if (!config_.is_threaded())
            analytics_.on_event(mkt_ptr);
        event_count++;

        if (order_opt)
        {
            order_opt->set_order_id(OrderIdGenerator::next());

            // Stop/stop-limit orders are buffered until triggered
            if (order_opt->get_order_type() == order_type::stop ||
                order_opt->get_order_type() == order_type::stop_limit)
            {
                pending_stops.push_back(order_pool_.acquire(*order_opt));
            }
            else if (config_.latency_model)
            {
                auto latency = config_.latency_model->get_order_latency();
                order_opt->set_earliest_eligible_ts(sim_time + latency);
                pending_orders.push({order_pool_.acquire(*order_opt), order_seq++});
            }
            else
            {
                // No latency model — process immediately (current behavior)
                order_opt->set_earliest_eligible_ts(sim_time);
                process_order(order_pool_.acquire(*order_opt));
            }
        }

        {
            auto now_report = std::chrono::steady_clock::now();
            if ((i + 1) == n || now_report - last_report_time >= std::chrono::milliseconds(200))
            {
                const double progress = ((i + 1) * 100.0) / static_cast<double>(n);
                std::cout << "\rProgress: " << std::fixed << std::setprecision(3) << progress
                          << "% | Trades executed: " << portfolio_.get_total_trades()
                          << std::flush;
                last_report_time = now_report;
            }
        }
    }

    // Drain any remaining pending orders after all market data
    while (!pending_orders.empty())
    {
        auto entry = pending_orders.top();
        pending_orders.pop();
        process_order(entry.order);
    }

    // Cancel all day orders at session end
    for (const auto& [symbol, oid] : day_order_ids)
    {
        auto ob = orderbook_registry_.get(symbol);
        if (ob)
            ob->cancel_order(oid);
    }

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << std::endl;
    std::cout << "Trades executed: " << portfolio_.get_total_trades()
              << " in " << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

    double throughput = static_cast<double>(event_count) / (elapsed_ms / 1000.0);
    std::cout << "Event throughput: " << throughput << " events/second" << std::endl;

    if (event_logger_) event_logger_->flush();
    stop_workers();
}

void engine::run_tick_data()
{
    if (!config_.event_log_path.empty() && !event_logger_)
        event_logger_ = std::make_unique<EventLogger>(config_.event_log_path);

    start_workers();

    const auto& ticks = data_handler_->tick_data;
    const auto n = ticks.size();
    const auto start = std::chrono::high_resolution_clock::now();

    std::cout << "\rProgress: 0.000% | Trades executed: 0" << std::flush;

    std::size_t event_count = 0;
    auto last_report_time = std::chrono::steady_clock::now();

    // Bar aggregator: converts ticks into OHLCV bars for on_market()
    BarAggregator bar_agg(std::chrono::seconds(1), [&](const market_event& bar)
    {
        auto bar_ptr = market_pool_.acquire(bar);
        auto order_opt = strategy_->on_market(bar);
        publish_event(bar_ptr);
        if (!config_.is_threaded())
            analytics_.on_event(bar_ptr);

        if (order_opt)
        {
            order_opt->set_order_id(OrderIdGenerator::next());
            order_opt->set_earliest_eligible_ts(bar.get_timestamp());

            auto adapter = get_adapter(bar.get_symbol());

            if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
                local->set_mid_price(last_mid_price_);

            adapter->submit_order(*order_opt);

            auto order_ptr = order_pool_.acquire(*order_opt);
            publish_event(order_ptr);
            if (!config_.is_threaded())
                analytics_.on_event(order_ptr);

            std::vector<fill_event> fills;
            if (adapter->poll_fills(fills))
            {
                for (auto& f : fills)
                {
                    auto fill_ptr = fill_pool_.acquire(f);
                    portfolio_.on_fill(f);
                    strategy_->set_position_open(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                    publish_event(fill_ptr);
                    if (!config_.is_threaded())
                        analytics_.on_event(fill_ptr);
                    event_count++;
                }
            }
            event_count++;
        }
    });

    for (std::size_t i = 0; i < n; ++i)
    {
        const auto& tick = ticks[i];

        // Convert data_tick_side to tick_side for event
        tick_side ts = tick_side::unknown;
        if (tick.side == data_tick_side::bid) ts = tick_side::bid;
        else if (tick.side == data_tick_side::ask) ts = tick_side::ask;

        tick_event te(tick.timestamp, tick.symbol, tick.price, tick.quantity, ts);

        // Track mid price
        last_mid_price_ = tick.price;

        // Publish tick event
        auto tick_ptr = tick_pool_.acquire(te);
        log_event(te);
        publish_event(tick_ptr);
        if (!config_.is_threaded())
            analytics_.on_event(tick_ptr);
        event_count++;

        // Dispatch to strategy's tick handler
        auto order_opt = strategy_->on_tick(te);
        if (order_opt)
        {
            order_opt->set_order_id(OrderIdGenerator::next());
            order_opt->set_earliest_eligible_ts(tick.timestamp);

            auto adapter = get_adapter(tick.symbol);

            if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
                local->set_mid_price(last_mid_price_);

            adapter->submit_order(*order_opt);

            auto order_ptr = order_pool_.acquire(*order_opt);
            publish_event(order_ptr);
            if (!config_.is_threaded())
                analytics_.on_event(order_ptr);

            std::vector<fill_event> fills;
            if (adapter->poll_fills(fills))
            {
                for (auto& f : fills)
                {
                    auto fill_ptr = fill_pool_.acquire(f);
                    portfolio_.on_fill(f);
                    strategy_->set_position_open(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                    publish_event(fill_ptr);
                    if (!config_.is_threaded())
                        analytics_.on_event(fill_ptr);
                    event_count++;
                }
            }
            event_count++;
        }

        // Feed tick into bar aggregator for bar-based strategies
        bar_agg.on_tick(tick.symbol, tick.price, tick.quantity, tick.timestamp);

        {
            auto now_report = std::chrono::steady_clock::now();
            if ((i + 1) == n || now_report - last_report_time >= std::chrono::milliseconds(200))
            {
                const double progress = ((i + 1) * 100.0) / static_cast<double>(n);
                std::cout << "\rProgress: " << std::fixed << std::setprecision(3) << progress
                          << "% | Trades executed: " << portfolio_.get_total_trades()
                          << std::flush;
                last_report_time = now_report;
            }
        }
    }

    // Flush any partial bar
    bar_agg.flush();

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << std::endl;
    std::cout << "Trades executed: " << portfolio_.get_total_trades()
              << " in " << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

    double throughput = static_cast<double>(event_count) / (elapsed_ms / 1000.0);
    std::cout << "Event throughput: " << throughput << " events/second" << std::endl;

    if (event_logger_) event_logger_->flush();
    stop_workers();
}

void engine::run_replay(const std::string& log_path)
{
    EventReplayer replayer(log_path);

    start_workers();

    const auto start = std::chrono::high_resolution_clock::now();
    std::size_t event_count = 0;

    std::cout << "\rReplay: processing events..." << std::flush;

    while (replayer.has_next())
    {
        auto ev = replayer.next();
        if (!ev) break;

        if (halt_flag_.load(std::memory_order_acquire))
            break;

        switch (ev->get_type()) {
        case event_type::market: {
            auto& mkt = static_cast<market_event&>(*ev);
            last_mid_price_ = mkt.get_close();

            // Replenish book
            auto ob = orderbook_registry_.get_or_create(mkt.get_symbol());
            market_maker_.replenish(ob, last_mid_price_);

            // Feed to strategy
            auto order_opt = strategy_->on_market(mkt);
            publish_event(ev);
            if (!config_.is_threaded())
                analytics_.on_event(ev);

            if (order_opt)
            {
                order_opt->set_order_id(OrderIdGenerator::next());
                order_opt->set_earliest_eligible_ts(mkt.get_timestamp());
                auto order_ptr = order_pool_.acquire(*order_opt);

                // Risk check
                auto snap = analytics_.snapshot();
                auto action = risk_manager_.check_order(*order_opt, portfolio_, snap);
                if (action == risk_action::reject) break;
                if (action == risk_action::halt) {
                    halt_flag_.store(true, std::memory_order_release);
                    break;
                }

                publish_event(order_ptr);
                if (!config_.is_threaded())
                    analytics_.on_event(order_ptr);

                auto adapter = get_adapter(mkt.get_symbol());
                if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
                    local->set_mid_price(last_mid_price_);
                adapter->submit_order(*order_opt);

                std::vector<fill_event> fills;
                if (adapter->poll_fills(fills))
                {
                    for (auto& f : fills)
                    {
                        auto fill_ptr = fill_pool_.acquire(f);
                        portfolio_.on_fill(f);
                        strategy_->set_position_open(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                        publish_event(fill_ptr);
                        if (!config_.is_threaded())
                            analytics_.on_event(fill_ptr);
                    }
                }
            }
            break;
        }
        case event_type::tick: {
            auto& te = static_cast<tick_event&>(*ev);
            last_mid_price_ = te.get_price();
            publish_event(ev);
            if (!config_.is_threaded())
                analytics_.on_event(ev);

            auto order_opt = strategy_->on_tick(te);
            if (order_opt)
            {
                order_opt->set_order_id(OrderIdGenerator::next());
                order_opt->set_earliest_eligible_ts(te.get_timestamp());
                auto adapter = get_adapter(te.get_symbol());
                if (auto* local = dynamic_cast<LocalBookAdapter*>(adapter.get()))
                    local->set_mid_price(last_mid_price_);
                adapter->submit_order(*order_opt);

                auto order_ptr = order_pool_.acquire(*order_opt);
                publish_event(order_ptr);
                if (!config_.is_threaded())
                    analytics_.on_event(order_ptr);

                std::vector<fill_event> fills;
                if (adapter->poll_fills(fills))
                {
                    for (auto& f : fills)
                    {
                        auto fill_ptr = fill_pool_.acquire(f);
                        portfolio_.on_fill(f);
                        strategy_->set_position_open(f.get_symbol(), portfolio_.position_open(f.get_symbol()));
                        publish_event(fill_ptr);
                        if (!config_.is_threaded())
                            analytics_.on_event(fill_ptr);
                    }
                }
            }
            break;
        }
        default:
            // Other event types (signal, l2, etc.) just pass through analytics
            publish_event(ev);
            if (!config_.is_threaded())
                analytics_.on_event(ev);
            break;
        }

        event_count++;
    }

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << std::endl;
    std::cout << "Replay complete: " << event_count << " events in "
              << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

    stop_workers();
}
