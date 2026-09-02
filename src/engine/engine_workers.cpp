// Engine worker/ring lifecycle: thread pinning, start/stop of the logging,
// risk, stats, observer, risk-stats and market-maker workers, provider
// callback revocation, and QuestDB persistence activation/tick.
// Extracted mechanically from engine.cpp (Phase 1 TU split); behavior unchanged.
#include "engine.h"
#include "live_safety_session.h"
#include "execution/async_support.h"
#include "reproducibility/deterministic_seed.h"
#ifdef HAS_QUESTDB
#include "data/questdb/run_tag.h"
#endif

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

std::unique_ptr<LoggingWorker> engine::make_logging_worker()
{
    auto text_sink = LoggingWorker::log_sink::none;
    if (config_.log_to_stdout)
        text_sink = LoggingWorker::log_sink::stdout_sink;
    else if (!config_.text_log_path.empty())
        text_sink = LoggingWorker::log_sink::file_sink;

    return std::make_unique<LoggingWorker>(
        config_.event_log_path, text_sink, config_.text_log_path, config_.compress_log,
        config_.log_max_bytes, config_.log_max_files);
}

void engine::pin_event_loop_thread()
{
    if (!config_.is_threaded() || config_.disable_pinning)
        return;

    int core_id = config_.pin_event_loop;
    if (core_id < 0)
    {
        auto core_map = build_core_map();
        for (const auto& ca : core_map)
            if (ca.role == core_role::event_loop) { core_id = ca.core_id; break; }
    }

    pin_current_thread(core_id);
}

void engine::start_workers()
{
    // Worker/ring orchestration. See core/docs/internal/engine-decomposition.md Wave 4 (E-60) + engine-decomposition.
    // Will be delegated to WorkerOrchestrator (rings, pinning, start/stop, drops).
    if (halt_flag_.load(std::memory_order_acquire))
    {
        provider_callbacks_armed_.store(false, std::memory_order_release);
        if (callbacks_armed_flag_)
            callbacks_armed_flag_->store(false, std::memory_order_release);
        return;
    }
    provider_callbacks_armed_.store(true, std::memory_order_release);
    if (callbacks_armed_flag_)
        callbacks_armed_flag_->store(true, std::memory_order_release);
    worker_failed_.store(false, std::memory_order_release);

    auto wire_failure = [this](Worker& w) {
        w.set_failure_flag(worker_failed_);
        w.set_failure_callback(
            [this](std::string_view reason) { trigger_halt(reason); });
        w.set_spin_policy(config_.worker_spin_policy);
        const bool durable_logging = !config_.event_log_path.empty()
            && std::string_view(w.worker_name()) == "logging";
        w.set_max_consecutive_errors(
            durable_logging ? 1U : config_.max_consecutive_worker_errors);
    };

    if (!config_.is_threaded())
    {
        (void)drain_provider_funding_updates();
        return;
    }

    auto core_map = build_core_map();

    auto find_core = [&](core_role role) -> int {
        if (config_.disable_pinning) return -1;
        switch (role) {
        case core_role::event_loop: if (config_.pin_event_loop >= 0) return config_.pin_event_loop; break;
        case core_role::logging:    if (config_.pin_logging >= 0)    return config_.pin_logging; break;
        case core_role::risk:       if (config_.pin_risk >= 0)       return config_.pin_risk; break;
        case core_role::stats:      if (config_.pin_stats >= 0)      return config_.pin_stats; break;
        case core_role::market_maker: if (config_.pin_mm >= 0)       return config_.pin_mm; break;
        }
        for (const auto& ca : core_map)
            if (ca.role == role) return ca.core_id;
        return -1;
    };

    switch (config_.threading)
    {
    case thread_preset::inline_mode:
        return;

    case thread_preset::logging_only:
    {
        logging_ring_ = std::make_shared<EventRing>();
        logging_worker_ = make_logging_worker();
        wire_failure(*logging_worker_);
        worker_threads_.emplace_back([this]() {
            logging_worker_->run(*logging_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::logging));
        break;
    }

    case thread_preset::light:
    {
        observer_ring_ = std::make_shared<EventRing>();
        observer_worker_ = std::make_unique<ObserverWorker>(risk_manager_, halt_flag_, order_tracker_.active_count_atomic(),
            config_.initial_balance, config_.rolling_window, config_.risk_free_rate,
            config_.periods_per_year, config_.max_equity_points,
            [this](std::string_view reason) { trigger_halt(reason); },
            config_.mode != engine_mode::backtest);
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
        risk_stats_worker_ = std::make_unique<RiskStatsWorker>(risk_manager_, halt_flag_, order_tracker_.active_count_atomic(),
            config_.initial_balance, config_.rolling_window, config_.risk_free_rate,
            config_.periods_per_year, config_.max_equity_points,
            [this](std::string_view reason) { trigger_halt(reason); },
            config_.mode != engine_mode::backtest);
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
        risk_worker_ = std::make_unique<RiskWorker>(risk_manager_, halt_flag_, order_tracker_.active_count_atomic(),
            config_.initial_balance, config_.rolling_window, config_.risk_free_rate,
            config_.periods_per_year, config_.max_equity_points,
            [this](std::string_view reason) { trigger_halt(reason); },
            config_.mode != engine_mode::backtest);
        stats_worker_ = std::make_unique<StatsWorker>(config_.initial_balance, 1000,
            config_.rolling_window, config_.risk_free_rate,
            config_.periods_per_year, config_.max_equity_points);
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
        logging_ring_ = std::make_shared<EventRing>();
        risk_ring_ = std::make_shared<EventRing>();
        stats_ring_ = std::make_shared<EventRing>();

        // Backtest determinism: the async MM worker replenishes the book on
        // its own schedule, so the book state at any bar — and therefore
        // fill prices — would depend on thread scheduling. Backtests use
        // the inline replenish path instead (same as the other presets);
        // the async worker stays available for shadow/live.
        const bool async_mm = (config_.mode != engine_mode::backtest);
        if (async_mm)
        {
            mm_ring_ = std::make_shared<EventRing>();
            mm_order_ring_ = std::make_shared<MMRing>();
        }

        logging_worker_ = make_logging_worker();
        risk_worker_ = std::make_unique<RiskWorker>(risk_manager_, halt_flag_, order_tracker_.active_count_atomic(),
            config_.initial_balance, config_.rolling_window, config_.risk_free_rate,
            config_.periods_per_year, config_.max_equity_points,
            [this](std::string_view reason) { trigger_halt(reason); },
            config_.mode != engine_mode::backtest);
        stats_worker_ = std::make_unique<StatsWorker>(config_.initial_balance, 1000,
            config_.rolling_window, config_.risk_free_rate,
            config_.periods_per_year, config_.max_equity_points);
        if (async_mm)
        {
            mm_worker_ = std::make_unique<MarketMakerWorker>(
                truetest::reproducibility::DeterministicSeedDeriver(config_.seed)
                    .derive(truetest::reproducibility::SeedDomain::market_maker),
                *mm_order_ring_,
                mm_calibration{config_.mm_levels_per_side,
                               config_.mm_base_depth,
                               config_.mm_base_spread_pct,
                               config_.mm_vol_spread_mult,
                               config_.mm_max_half_spread_pct,
                               config_.qty_scale});
            mm_threaded_ = true;
        }
        wire_failure(*logging_worker_);
        wire_failure(*risk_worker_);
        wire_failure(*stats_worker_);
        if (mm_worker_) wire_failure(*mm_worker_);

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

        if (mm_worker_)
        {
            worker_threads_.emplace_back([this]() {
                mm_worker_->run(*mm_ring_);
            });
            pin_to_core(worker_threads_.back(), find_core(core_role::market_maker));
        }
        break;
    }
    }

    // Workers and all rings are live now. Apply retained pre-start settlements
    // before the first market/strategy event so persistence, worker analytics,
    // the engine risk view, and Portfolio observe the same funding sequence.
    (void)drain_provider_funding_updates();

}

void engine::revoke_provider_callbacks()
{
    if (!config_.provider) return;

    if (auto adapter = config_.provider->get_execution_adapter())
    {
        if (auto* cap = adapter->get_async_support())
            cap->clear_unknown_fill_handler();
    }
    config_.provider->set_halt_callback([](std::string_view){});
}

void engine::stop_workers()
{
    // Revoke callbacks, then hand provider quiesce/kill/finish to the shared
    // exact-once LiveSafetySession before joining engine-owned workers.
    provider_callbacks_armed_.store(false, std::memory_order_release);
    if (callbacks_armed_flag_) callbacks_armed_flag_->store(false, std::memory_order_release);

    // Early drain so worker releases are visible before we revoke callbacks
    // and join. Helps ensure in_use() is low before pools/rings are torn down.
    drain_object_pool_returns();

    // Stop watchdog early (its poll thread holds a callback into us).
    // Do this right after disarm + before joins and heavy teardown.
    if (worker_watchdog_) worker_watchdog_->stop();

    const auto reason = halt_flag_.load(std::memory_order_acquire)
        ? live_shutdown_reason::engine_halt
        : live_shutdown_reason::normal_end;
    (void)finalize_live_shutdown(reason);

    // Provider shutdown above joins the private account-stream producer.  The
    // ring is now stable, so record every admitted settlement before flushing
    // persistence or stopping worker consumers.
    (void)drain_provider_funding_updates();

    // QuestDB: give the final funding drain and all earlier audit enqueues a
    // last flush opportunity before worker/ring teardown. Best-effort; strict
    // persistence has already latched its own terminal failure.
#ifdef HAS_QUESTDB
    if (questdb_store_) {
        (void)flush_funding_audit();
        try { questdb_store_->flush(); } catch (...) {}
    }
#endif

    // Provider quiesce above stops DMS/transport callback threads before
    // their std::function targets are replaced, avoiding callback races/UAF.
    revoke_provider_callbacks();

    if (observer_worker_) observer_worker_->stop();
    if (logging_worker_) logging_worker_->stop();
    if (risk_worker_) risk_worker_->stop();
    if (stats_worker_) stats_worker_->stop();
    if (risk_stats_worker_) risk_stats_worker_->stop();
    if (mm_worker_) mm_worker_->stop();

    for (auto& t : worker_threads_)
    {
        if (t.joinable())
            t.join();
    }
    worker_threads_.clear();

    if (logging_worker_)
    {
        try
        {
            logging_worker_->finalize();
        }
        catch (const std::exception& e)
        {
            worker_failed_.store(true, std::memory_order_release);
            trigger_halt(e.what());
        }
        catch (...)
        {
            worker_failed_.store(true, std::memory_order_release);
            trigger_halt("durable event-log finalization failed");
        }
    }

    // Workers joined — safe to stamp research counters onto export analytics.
    fold_research_counters_into_export_analytics();

    // Best-effort drain of rings after workers are joined. Any remaining
    // references held by external ring shared_ptr copies (tests, UI, etc.)
    // may still drop later; their deleters are now protected by the
    // pool alive_ guards + armed callback guards.
    auto drain_ring = [](const std::shared_ptr<EventRing>& r) {
        if (!r) return;
        event_pointer ev;
        while (r->try_pop(ev)) {}
    };
    drain_ring(logging_ring_);
    drain_ring(risk_ring_);
    drain_ring(stats_ring_);
    drain_ring(observer_ring_);
    drain_ring(risk_stats_ring_);
    drain_ring(mm_ring_);

    if (mm_order_ring_)
    {
        event_pointer ev; // different ring type, keep original loop
        while (mm_order_ring_->try_pop(ev)) {}
    }

    Worker* all_workers[] = {
        logging_worker_.get(), risk_worker_.get(), stats_worker_.get(),
        observer_worker_.get(), risk_stats_worker_.get(), mm_worker_.get(),
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
                  << " mm=" << mm_drops_
                  << "\n";
    }

    auto report_hwm = [](const char* name, const std::shared_ptr<EventRing>& ring) {
        if (!ring) return;
        auto hwm = ring->high_watermark();
        if (hwm > 0)
        {
            double pct = hwm * 100.0 / ring->capacity();
            std::cerr << "  Ring HWM: " << name << "=" << hwm
                      << "/" << ring->capacity() << " (" << static_cast<int>(pct) << "%)\n";
        }
    };
    report_hwm("logging", logging_ring_);
    report_hwm("risk", risk_ring_);
    report_hwm("stats", stats_ring_);
    report_hwm("observer", observer_ring_);
    report_hwm("risk_stats", risk_stats_ring_);
    report_hwm("mm", mm_ring_);
}

#ifdef HAS_QUESTDB
void engine::questdb_begin()
{
    if (!config_.persist_enabled) return;
    if (questdb_active_) return;

    truetest::questdb::StoreConfig scfg;
    scfg.host = config_.questdb_host;
    scfg.ilp_port = config_.questdb_ilp_port;
    scfg.http_port = config_.questdb_http_port;
    try
    {
        scfg.run_tag = truetest::questdb::make_run_tag(config_.run_tag);
    }
    catch (const std::exception& e)
    {
        std::cerr << "  WARNING: invalid --run-tag (" << e.what()
                  << ") — persistence disabled.\n";
        if (config_.questdb_strict)
            throw std::runtime_error("strict persistence rejected invalid run tag");
        return;
    }

    scfg.mode = (config_.mode == engine_mode::backtest ? "backtest"
              :  config_.mode == engine_mode::shadow   ? "shadow"
              :                                          "live");
    // Engine_core has no TT_TARGET define, so derive a binary label from
    // the runtime mode. main.inc could supply a more accurate value, but
    // mode-based naming is informative enough for runs_meta.
    scfg.binary = (config_.mode == engine_mode::backtest ? "engine_backtest"
                :  config_.mode == engine_mode::shadow   ? "engine_shadow"
                :                                          "engine_live");
    scfg.strategy = primary_strategy_name_;
    if (data_handler_ && data_handler_->has_bar_data())
        scfg.symbol = data_handler_->first_symbol();
    scfg.initial_equity = config_.initial_balance;
    scfg.notes = config_.run_notes;
    scfg.strict = config_.questdb_strict;
    scfg.ttl_days = 0; // Phase 4: can be extended to config if needed (default no TTL)

    if (config_.questdb_strict && !config_.questdb_fallback_path.empty())
        scfg.fallback_path = config_.questdb_fallback_path;
    else if (config_.questdb_strict && !scfg.run_tag.empty())
    {
        // Auto-generate a sensible fallback filename next to the binary log if possible
        scfg.fallback_path = scfg.run_tag + ".questdb_fallback.ilp";
    }

    questdb_store_ = std::make_shared<truetest::questdb::QuestdbStore>(
        std::move(scfg));
    if (config_.questdb_strict)
    {
        questdb_store_->set_strict_failure_callback([this] {
            run_failed_.store(true, std::memory_order_release);
            if (config_.mode != engine_mode::backtest)
                trigger_halt("strict persistence failure");
        });
    }

    if (questdb_store_->begin())
    {
        questdb_active_ = true;
        // Set to "now - cadence" so the very first maybe_questdb_tick() after activation
        // will consider flushing promptly (improves reliability for short runs / early data).
        last_questdb_flush_ = std::chrono::steady_clock::now() - config_.questdb_flush_cadence;
        std::cerr << "  QuestDB persistence ENABLED (run_tag="
                  << questdb_store_->run_tag() << ")\n";
        // Single canonical place that decides the concrete sink (ctor starts with Noop).
        // No raw if(active && store) remains in caller paths.
        audit_sink_ = std::make_shared<QuestdbOrderAuditSink>(questdb_store_, &questdb_active_);
    }
    else
    {
        if (config_.questdb_strict)
        {
            std::cerr << "\n  FATAL (strict mode): QuestDB unreachable at "
                      << config_.questdb_host << ":" << config_.questdb_http_port << "\n"
                      << "  --persist-strict requires a working QuestDB instance.\n"
                      << "  Start QuestDB (e.g. `questdb start`) and retry, or remove --persist-strict.\n\n";
            throw std::runtime_error("strict persistence startup failed");
        }
        else
        {
            std::cerr << "  WARNING: QuestDB unreachable at "
                      << config_.questdb_host << ":" << config_.questdb_http_port
                      << " — continuing with persistence DISABLED for this session.\n"
                      << "  Start the daemon with: questdb start\n"
                      << "  Or re-run without --persist to suppress this warning.\n";
            questdb_store_.reset();
        }
    }
}

void engine::questdb_end()
{
    if (!questdb_active_ || !questdb_store_) return;
    (void)flush_funding_audit();
    const auto report = analytics_.snapshot();
    double final_equity;
    {
        std::lock_guard<std::mutex> lk(last_mark_prices_mu_);
        final_equity = portfolio_.get_equity(
            last_mark_prices_, last_mid_price_.load(std::memory_order_relaxed));
    }
    const std::size_t rejs = audit_sink_ ? audit_sink_->total_rejections() : 0;
    // Prefer delegating through the seam when available (single place for persistence finalization).
    // See core/docs/internal/engine-decomposition.md Phase 2 (E-21) + engine-decomposition skill "QuestDB Isolation".
    // The else branch below is legacy fallback only (should be unreachable when questdb_active_).
    // All recording paths use audit_sink_; finalize should too.
    if (audit_sink_)
    {
        audit_sink_->finalize_run(final_equity,
                                  report.total_orders,
                                  report.total_fills,
                                  rejs,
                                  report.max_drawdown,
                                  report.sharpe_ratio,
                                  report.sortino_ratio,
                                  report.profit_factor,
                                  report.win_rate,
                                  report.calmar_ratio,
                                  report.total_trades,
                                  report.winning_trades);
    }
    check_strict_persistence();
    // Note: legacy direct questdb_store_ finalize path removed in Phase 2 prep
    // (core/docs/internal/engine-decomposition.md#E-21) to enforce single IOrderAuditSink seam.
    // Activation in questdb_begin always sets a real sink when store is active.
    questdb_active_ = false;
}
#endif

void engine::maybe_questdb_tick()
{
#ifdef HAS_QUESTDB
    if (!questdb_active_ || !questdb_store_) return;
    auto now = std::chrono::steady_clock::now();
    if (now - last_questdb_flush_ >= config_.questdb_flush_cadence) {
        (void)flush_funding_audit();
        questdb_store_->tick();
        last_questdb_flush_ = now;
        check_strict_persistence();
    }
#endif
}

#ifdef HAS_QUESTDB
bool engine::flush_funding_audit() noexcept
{
    provider_funding_update record;
    while (funding_audit_ring_.try_pop(record))
    {
        try
        {
            const auto ts = std::chrono::system_clock::time_point{
                std::chrono::milliseconds{record.event_time_ms}};
            funding_event event(ts, record.symbol_view(), 0.0,
                                record.cash_delta, "FUNDING_FEE");
            audit_sink_->record_funding(
                event, audit_sink_ ? audit_sink_->run_tag() : "");
        }
        catch (...)
        {
            trigger_halt("funding persistence staging drain failed");
            return false;
        }
    }
    return true;
}

void engine::check_strict_persistence()
{
    if (!config_.questdb_strict || !questdb_store_
        || !questdb_store_->strict_failure_latched())
        return;
    run_failed_.store(true, std::memory_order_release);
    if (config_.mode != engine_mode::backtest)
        trigger_halt("strict persistence failure");
}
#endif
