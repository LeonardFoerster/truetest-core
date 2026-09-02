#pragma once
#include <climits>
#include <cstdint>
#include "data/data_handler.h"
#include "strategy/strategy_interface.h"
#include "analytics/adverse_selection_tracker.h"
#include "execution/portfolio.h"
#include "execution/execution_adapter.h"
#include "execution/instrument.h"
#include "execution/order_tracker.h"
#include "orderbook/orderbook.h"
#include "orderbook/orderbook_registry.h"
#include "market_maker/market_maker.h"
#include "analytics/analytics.h"
#include "analytics/bar_aggregator.h"
#include "risk/risk_manager.h"
#include "risk/futures_risk_check.h"
#include "threading/worker_watchdog.h"
#include "threading/ring_buffer.h"
#include "threading/thread_config.h"
#include "logging_worker.h"
#include "risk_worker.h"
#include "stats_worker.h"
#include "observer_worker.h"
#include "risk_stats_worker.h"
#include "market_maker_worker.h"
#include "engine_config.h"

namespace truetest::ui { struct streaming_stats; }
#include "ui/dashboard_snapshot.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include "core/event.h"
#include "core/event_log.h"
#include "types/order_id.h"
#include "types/control_block_pool.h"
#include "types/object_pool.h"
#include "types/pool_exhausted.h"
#include "exits/exit_manager.h"

#include "debug/stage_timer.h"

#include "order_audit_sink.h"
#include "execution_router.h"
#include "instrument_spec_cache.h"
#include "checkpoint.h"
#include "dashboard_snapshot_builder.h"
#include "order_attribution_store.h"
#include "pending_order_scheduler.h"
#include "risk_unwind_sink.h"
#include "fill_processor.h"
#include "engine_hotpath_sink.h"
#include "order_intent_processor.h"

#ifdef HAS_QUESTDB
#include "data/questdb/store.h"
#endif

#ifdef HAS_DEBUG
#include "debug/debug_log.h"
#include "debug/memory_info.h"
#include "debug/ring_stats.h"
#include "debug/debug_report.h"
#endif

#include "providers/data_bridge.h"
#include "providers/provider_event.h"
#include "providers/local/csv_parser.h"
#include "providers/provider_event.h"
#include "analytics/shadow_tracker.h"
#include "strategy/strategy_factory.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

static constexpr std::size_t DEFAULT_RING_SIZE = 65536;

using EventRing = RingBuffer<event_pointer, DEFAULT_RING_SIZE>;

enum class live_shutdown_reason;

// ============================================================
// LIVE-SAFETY SURFACE — Phase 1 freeze (see engine.cpp for the full banner
// and scripts/check-live-safety-freeze.sh for the authoritative path list).
//
// Engine decomposition Phase 1 (mechanical translation-unit split, 2026-08):
// this header's declarations are unchanged; only where each `engine::`
// method is *defined* moved. engine.cpp keeps ctor/dtor, log_event,
// publish_event, trigger_halt, request_operator_kill, finalize_live_shutdown,
// and run(). Every other private/public method below is now defined in one
// of: engine_lifecycle.cpp, engine_market.cpp, engine_orders.cpp,
// engine_fills.cpp, engine_workers.cpp, engine_observability.cpp, or (pre-
// existing) engine_pending.cpp — grouped by responsibility, not alphabetized.
// See the map comment at the top of engine.cpp for the full breakdown.
// ============================================================

// Implements IEngineHotPathSink (log_event/publish_event/trigger_halt) so
// OrderIntentProcessor — and future domain processors — can reach these
// centralized primitives through a narrow interface reference instead of a
// concrete engine& back-reference. The three methods below already exist as
// engine's own hot-path primitives; this only adds `override` to them. See
// engine_hotpath_sink.h and the "OrderIntentProcessor Preparation Report" §9.
//
// Also implements IRiskUnwindSink so FillProcessor can request emergency
// liquidation without a concrete OrderIntentProcessor& (which does not exist
// yet at FillProcessor's construction time) — see risk_unwind_sink.h for the
// construction-order proof. request_unwind() forwards to
// orders_->unwind_positions(...), deref'd only when actually invoked.
class engine final : private IEngineHotPathSink, private IRiskUnwindSink
{
private:
    engine_config config_;
    std::shared_ptr<data_handler> data_handler_;
    OrderbookRegistry orderbook_registry_;
    std::shared_ptr<IStrategy> strategy_;
    std::vector<std::shared_ptr<IStrategy>> additional_strategies_;
    std::vector<std::string> additional_strategy_names_;
    std::string primary_strategy_name_;
    std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>> execution_adapters_;
    portfolio portfolio_;
    std::optional<portfolio> exchange_portfolio_;     // only used in shadow mode: real tape view
    OrderTracker order_tracker_;
    Analytics analytics_;
    std::optional<Analytics> exchange_analytics_;     // only used in shadow mode
    AdverseSelectionTracker adverse_selection_;

    void write_adapter_diagnostics(truetest::ui::streaming_stats& st);
    void refresh_top_of_book_atomics(const orderbook& ob);
    RiskManager risk_manager_;
    // Optional venue-specific pre-trade gate. Stashed from
    // provider->get_risk_check() at construct time; null when the
    // provider doesn't supply one (spot, backtest providers, etc.).
    std::shared_ptr<IRiskCheck> risk_check_;
    // Optional liveness watchdog. Created in the constructor only if
    // the provider returns at least one liveness_source - currently
    // only the futures dead-man's switch heartbeat opts in.
    std::unique_ptr<WorkerWatchdog> worker_watchdog_;
    MarketMaker market_maker_;
    // Written from event processing thread(s), read by cold snapshot builder,
    // risk checks, adapters, logging. Atomic for price to avoid data race
    // surface on plain double& held by builder (see memory check 2026-07-18).
    alignas(64) std::atomic<double> last_mid_price_{0.0};
    std::string last_mark_symbol_;
    // Per-symbol last marks for multi-symbol portfolio equity (FR-06).
    // Reserve/clear at run start — never first-touch rehash on the hot mark path.
    // Unlike last_mid_price_ (atomic<double>), this is a plain map: it can
    // still insert on an unreserved symbol (rehash) while a snapshot reader
    // is mid-iteration/find. The web server's poller thread reads this
    // concurrently with the event-loop thread via DashboardSnapshotBuilder
    // (see web/web_server.h) — a real cross-thread access, not hypothetical.
    // Guard every read/write with last_mark_prices_mu_.
    mutable std::mutex last_mark_prices_mu_;
    // R3: value carries the observation timestamp so risk can classify the
    // mark as valid/stale/missing instead of marking to an arbitrarily old
    // price. Stamped from the sim clock, never wall clock (determinism).
    std::unordered_map<std::string, mark_point> last_mark_prices_;
    // Last market/tick sim timestamp for cancel/amend audit (EL-CANCEL-WALLCLOCK).
    // Updated on every bar/tick event path; cancel_event uses this, not wall clock.
    std::chrono::system_clock::time_point last_sim_time_{};
    // F-08: the instant the information behind the current observation
    // existed. Equal to last_sim_time_ for ticks and L2 updates (which are
    // point observations); one bar interval later for bars, whose timestamp
    // is the OPEN and whose close is what a strategy actually reacts to.
    std::chrono::system_clock::time_point last_decision_ts_{};
    // Inferred once per run from the loaded series. Zero when it cannot be
    // inferred, which leaves the decision timestamp equal to the bar open —
    // i.e. exactly the pre-F-08 behaviour, never a guess.
    std::chrono::system_clock::duration bar_interval_{};

    std::size_t data_rows_rejected_{0};

    // Instrument spec cache (moved out; engine delegates). Cold path.
    std::unique_ptr<InstrumentSpecCache> instrument_spec_cache_;

    // Symbols already carrying real L2 depth - MarketMaker::replenish is
    // suppressed here so paper liquidity can't corrupt the fill sim.
    std::unordered_set<std::string> l2_seeded_symbols_;
    struct l2_sequence_state
    {
        std::uint64_t last_update_id{0};
        bool bootstrap_pending{false};
        bool present{false};
    };
    // Indexed by the orderbook registry's bounded dense SymbolTable id.
    // This avoids a second node/string allocation on the L2 event path.
    std::array<l2_sequence_state, SymbolTable::kMaxSymbols>
        l2_sequence_states_{};
    // resolve_instrument_spec / apply_instrument_spec moved to
    // OrderIntentProcessor (Phase 2) — route()'s own helpers now.

    std::unique_ptr<BarAggregator> tick_aggregator_;
    std::chrono::milliseconds tick_bar_interval_{60000};

    ObjectPool<market_event>      market_pool_;
    ObjectPool<order_event>       order_pool_;
    ObjectPool<fill_event>        fill_pool_;
    ObjectPool<tick_event>        tick_pool_;
    ObjectPool<l2_update_event>   l2_update_pool_;
    ObjectPool<l2_snapshot_event> l2_snapshot_pool_;
    ObjectPool<rejection_event>   rejection_pool_;
    ObjectPool<cancel_event>      cancel_pool_;
    ObjectPool<amend_event>       amend_pool_;
    ObjectPool<funding_event>     funding_pool_;
    ControlBlockPool              control_block_pool_;

    void prewarm_object_pools();
    // Phase 3: reclaim worker-thread pool releases before hot-path acquire.
    void drain_object_pool_returns() noexcept;

    template<typename T, std::size_t BlockSize, typename... Args>
    std::shared_ptr<T> acquire_pooled(ObjectPool<T, BlockSize>& pool, Args&&... args)
    {
        try
        {
            return pool.acquire(std::forward<Args>(args)...);
        }
        catch (const pool_exhausted& e)
        {
            trigger_halt(e.what());
            throw;
        }
    }

    std::shared_ptr<EventRing> logging_ring_;
    std::shared_ptr<EventRing> risk_ring_;
    std::shared_ptr<EventRing> stats_ring_;
    std::shared_ptr<EventRing> observer_ring_;
    std::shared_ptr<EventRing> risk_stats_ring_;
    std::shared_ptr<EventRing> mm_ring_;
    std::shared_ptr<MMRing> mm_order_ring_;

    void publish_event(const event_pointer& ev) override;

    // IRiskUnwindSink override — forwards to orders_->unwind_positions(...).
    // Defined out-of-line in engine.cpp (after orders_ is declared), matching
    // log_event/publish_event's declared-here/defined-in-.cpp convention. Safe
    // to call any time after construction completes; a defensive call before
    // orders_ exists latches the same terminal post-fill-risk halt rather than
    // silently skipping an emergency unwind.
    void request_unwind(std::size_t& event_count) override;

#ifdef HAS_QUESTDB
    std::shared_ptr<truetest::questdb::QuestdbStore> questdb_store_;
    bool questdb_active_ = false;  // true only after successful begin()
    RingBuffer<provider_funding_update, 256> funding_audit_ring_;

    // Last time we called tick() for time-based ILP flushing (Phase 1 hardening).
    std::chrono::steady_clock::time_point last_questdb_flush_{};

    void questdb_begin();
    void questdb_end();
    bool flush_funding_audit() noexcept;
    void check_strict_persistence();
#endif

    // Declared unconditionally (guarded impl) to eliminate #ifdef guards from hot paths.
    // Does nothing if persist is not active.
    void maybe_questdb_tick();

    // New seams from engine-decomposition (PR-03 wiring).
    // See core/docs/internal/engine-decomposition.md Phase 2 (E-20/E-21) + ~/.grok/skills/engine-decomposition/SKILL.md.
    // - audit_sink_: single seam for *all* order/fill/reject/cancel/amend/funding/event recording.
    //   Engine calls ONLY record_* methods. No questdb_store_ inspection for decisions.
    // - router_: adapter resolution, submit/poll_fills, L2 forwarding, advance.
    // - router_: partial adapter seam. Resolution, basic submit/poll, L2 and
    //   advance delegate here; async submit-result and exchange-shadow paths
    //   remain characterized engine-owned bypasses pending extraction.
    std::shared_ptr<IOrderAuditSink> audit_sink_;
    std::unique_ptr<ExecutionRouter> router_;
    ProviderFundingIngress* provider_funding_ingress_ = nullptr;

    // Dashboard logic extracted to cold collaborator (Wave 1).
    // See core/docs/internal/engine-decomposition.md (E-30..) + engine-decomposition skill.
    // Engine delegates public snapshot API and the refresh tick from publish_event.
    // Cache mutations from hot paths (fills, orders) now go through the builder.
    std::unique_ptr<DashboardSnapshotBuilder> dashboard_builder_;

    void write_checkpoint_if_due(std::size_t event_count);
    void restore_from_checkpoint();

    std::unique_ptr<CheckpointManager> checkpoint_mgr_;

    std::unique_ptr<EventLogger> event_logger_;

    std::unique_ptr<ShadowTracker> shadow_tracker_;

    truetest::exits::ExitManager exit_manager_;

    // register_strategy_exit_intent / finalize_strategy_route / evaluate_exits
    // (both overloads) / route_order / sweep_resting_limits /
    // check_pending_stops / deliver_mm_book_trades moved to
    // OrderIntentProcessor (Phase 2 — see order_intent_processor.{h,cpp} and
    // the orders_ member declaration below). exit_manager_ stays engine-owned
    // (the canonical exit-policy/state owner) and is referenced by
    // OrderIntentProcessor, not duplicated. Call sites here now say
    // orders_->finalize_route(...) / orders_->evaluate_exits(...) /
    // orders_->route(...) / orders_->sweep_resting_limits(...) /
    // orders_->check_pending_stops(...) / orders_->deliver_mm_book_trades(...).

    // Invoked by the engine on each fill-poll cycle to register any
    // venue-bracket-leg metadata produced by the unknown_fill_handler
    // installed on the provider's ExecutionBridge. Safe to call when
    // there is no bridge - it just no-ops.
    void drain_venue_bracket_meta();
    // drain_async_submit_results moved to OrderIntentProcessor; call sites
    // here now say orders_->drain_async_submit_results(...).
    // Sole consumer for provider funding ingress. Only this engine-thread path
    // may acquire funding_pool_, mutate portfolio/audit, or publish the event.
    bool drain_provider_funding_updates() noexcept;

    // Fill pipeline (stamp_fill_attribution, handle_engine_fill,
    // dispatch_fill_to_strategy, process_adapter_fills,
    // notify_position_change_all) extracted to FillProcessor — Phase 2 engine
    // decomposition (2026-08). See core/docs/internal/engine-decomposition.md
    // "Phase 2: Domain Processors" and the fills_ member below.

    void log_event(const event& ev) override;

    // process_order / unwind_positions moved to OrderIntentProcessor (Phase 1
    // of the domain-processor extraction that follows FillProcessor). See
    // orders_ member declaration below for the dependency/construction-order
    // rationale, and order_intent_processor.{h,cpp} for the implementation.
    // Call sites here now say orders_->process(...) / orders_->unwind_positions(...).

    void dispatch_extras_on_market(const market_event& mkt,
                                   const std::chrono::system_clock::time_point& ts,
                                   std::size_t& event_count);
    void dispatch_extras_on_tick(const tick_event& te,
                                 const std::chrono::system_clock::time_point& ts,
                                 std::size_t& event_count);
    // marked_account_equity moved to OrderIntentProcessor (public there,
    // unlike this former private method, since sync_strategy_account_equity
    // below is a second caller that stays engine-owned).
    void sync_strategy_account_equity(IStrategy& strategy) const;

    void process_single_bar(const bar_record& rec, std::size_t& event_count,
                            const std::chrono::system_clock::time_point& timestamp);

    void process_single_tick(const tick_record& rec, std::size_t& event_count);

    // pending_stops_ moved to OrderIntentProcessor (Phase 3) — every reader
    // and writer (route()'s staging push, check_pending_stops()'s
    // trigger/erase, cancel()'s search/erase) now lives there; canonical
    // ownership moved with them. clear_pending_state() below calls
    // orders_->clear_pending_stops() instead of touching a member directly.

    // Reused L2 adapter payloads. Reserved during cold initialization so a
    // snapshot does not allocate on the provider→engine hot path. Market-
    // domain, unrelated to order-attribution/pending-scheduling extraction.
    std::vector<std::pair<double, double>> l2_bid_scratch_;
    std::vector<std::pair<double, double>> l2_ask_scratch_;

    // pending_cancels_ moved to OrderIntentProcessor (Phase 3) — its sole
    // writer (cancel()) and sole reader/eraser (drain_async_submit_results())
    // both live there. ExecutionRouter's former reference to this map was
    // confirmed dead code (never read in any method body) and removed
    // rather than repointed — see execution_router.h.

    // OrderIntentProcessor preparatory extraction (2026-08): canonical owner
    // of order-attribution metadata (order_id -> opener_order_id/strategy_name,
    // former order_meta_ map) and deterministic latency/bar-delay pending-
    // order scheduling (former pending_orders_/bar_delayed_orders_/
    // bar_delayed_ready_/order_seq_/day_order_ids_). Both are narrow, zero-
    // engine-dependency leaf collaborators — see order_attribution_store.h /
    // pending_order_scheduler.h and the "OrderIntentProcessor Preparation
    // Report" §7/§8 for why each was extracted. Constructed in the ctor body
    // before router_/dashboard_builder_/fills_ (FillProcessor holds a
    // reference to *attribution_); declared here, before fills_, so both are
    // destroyed after it.
    std::unique_ptr<OrderAttributionStore> attribution_;
    std::unique_ptr<PendingOrderScheduler> pending_scheduler_;

    // register_order_meta/lookup_opener/lookup_strategy_name forwards
    // removed in Phase 3: their last remaining callers (route()'s
    // registration, cancel()'s/drain_final_pending's strategy-name lookups)
    // all moved into OrderIntentProcessor, which calls attribution_'s own
    // methods directly. The one surviving cold caller — the authoritative
    // ledger-replay path in engine_market.cpp — now calls
    // attribution_->register_order(order) directly (engine still owns the
    // OrderAttributionStore instance; no forward needed for one call site).

    // Fill pipeline — Phase 2 engine decomposition (2026-08). Constructed in
    // the ctor body after router_/dashboard_builder_/attribution_ are valid
    // (FillProcessor holds references to them); declared here (after all its
    // dependency members) so it is destroyed before them. See
    // core/docs/internal/engine-decomposition.md "Phase 2: Domain Processors".
    std::unique_ptr<FillProcessor> fills_;

    std::atomic<bool> halt_flag_{false};

    // pause_all_ and mm_threaded_ moved up here (2026-08, Phase 3 safety
    // review finding) so both are declared — and therefore destroyed —
    // *after* orders_ below, matching every other member orders_ holds by
    // reference. Previously both were declared after orders_ (pause_all_
    // grouped with the other atomic run-state flags, mm_threaded_ next to
    // mm_worker_), which meant they were destroyed *before* orders_ in
    // ~engine() — inert only because OrderIntentProcessor has no custom
    // destructor that touches them, but a latent use-after-destruction trap
    // for any future change. See pause_all_'s/mm_threaded_'s prior locations
    // below for the rest of their original context comments.
    std::atomic<bool> pause_all_{false};
    // Canonical same-thread MM replenish predicate. All synchronous replenish
    // gates (engine::run, process_single_bar, OrderIntentProcessor) use this
    // value rather than `!mm_worker_`, so processors do not depend on the
    // concrete MarketMakerWorker type and cannot race a live worker book if
    // the unique_ptr and flag ever diverge. Set exactly where mm_worker_ is
    // created (engine_workers.cpp); never reset, matching mm_worker_'s own
    // never-nulled lifecycle. Read only from the event-loop thread, after
    // start_workers() (which also runs on that thread) has returned — no
    // atomic needed.
    bool mm_threaded_ = false;

    // OrderIntentProcessor — Phase 1 of the domain-processor extraction that
    // follows FillProcessor. Owns the canonical normal order-submission path
    // (process_order/unwind_positions/drain_async_submit_results/
    // marked_account_equity). Constructed last in the ctor body (needs
    // *fills_, *router_, *attribution_, halt_flag_, pause_all_,
    // mm_threaded_, and every other collaborator above to already exist)
    // and declared last here — after every member it references — so it
    // is the first thing destroyed. See order_intent_processor.h and the
    // "OrderIntentProcessor Preparation Report" Phase-1 deliverable for the
    // full dependency/construction-order rationale.
    std::unique_ptr<OrderIntentProcessor> orders_;

    // Heap-allocated armed flag for provider callbacks.
    // Callbacks (which may fire after engine destruction) capture a
    // shared_ptr copy of this token so the "still armed?" check itself
    // is never a use-after-destruction of the engine object.
    //
    // Lifetime contract (see 2026-07-18 memory check): this flag + the
    // lifetime_ tokens in ObjectPools + explicit revoke/close/drain order
    // in stop_workers protect against in-flight callbacks touching
    // destroyed members. In-flight bodies that passed the load may still
    // execute; they must only touch things with independent lifetime
    // (shared rings) or guarded pools.
    std::shared_ptr<std::atomic<bool>> callbacks_armed_flag_ = std::make_shared<std::atomic<bool>>(false);

    // (kept for compatibility with some internal reads; the real authority is the flag above)
    std::atomic<bool> provider_callbacks_armed_{false};

    std::atomic<bool> worker_failed_{false};
    std::atomic<bool> run_failed_{false};
    // Authoritative replay is one-shot. A failed application can expose a
    // prefix internally, so the same engine object may never retry or replay
    // a second ledger on top of existing economic state.
    bool authoritative_replay_started_{false};
    std::atomic<bool> live_shutdown_failure_reported_{false};
    // pause_all_ moved up next to halt_flag_ (declared before orders_, see
    // above) — Phase 3 construction-order fix; was here originally.
    std::atomic<bool> flatten_request_{false};

    std::size_t logging_drops_ = 0;
    std::size_t risk_drops_ = 0;
    std::size_t stats_drops_ = 0;
    std::size_t observer_drops_ = 0;
    std::size_t risk_stats_drops_ = 0;
    std::size_t mm_drops_ = 0;

#ifdef HAS_DEBUG
    debug::StageTimer stage_timer_;
    debug::MemorySampler memory_sampler_;
    debug::ring_diagnostics logging_diag_{"logging_ring", DEFAULT_RING_SIZE};
    debug::ring_diagnostics risk_diag_{"risk_ring", DEFAULT_RING_SIZE};
    debug::ring_diagnostics stats_diag_{"stats_ring", DEFAULT_RING_SIZE};
    debug::ring_diagnostics observer_diag_{"observer_ring", DEFAULT_RING_SIZE};
    debug::ring_diagnostics risk_stats_diag_{"risk_stats_ring", DEFAULT_RING_SIZE};
    debug::ring_diagnostics mm_diag_{"mm_ring", DEFAULT_RING_SIZE};
#endif

    std::unique_ptr<LoggingWorker> logging_worker_;
    std::unique_ptr<RiskWorker> risk_worker_;
    std::unique_ptr<StatsWorker> stats_worker_;
    std::unique_ptr<ObserverWorker> observer_worker_;
    std::unique_ptr<RiskStatsWorker> risk_stats_worker_;
    std::unique_ptr<MarketMakerWorker> mm_worker_;
    // mm_threaded_ moved up next to halt_flag_/pause_all_ (declared before
    // orders_, see above) — Phase 3 construction-order fix; was here
    // originally, set exactly where mm_worker_ is created below
    // (engine_workers.cpp), never reset, matching mm_worker_'s own
    // never-nulled lifecycle.

    // Worker/ring lifecycle state.
    // Planned extraction Wave 4 (WorkerOrchestrator) per core/docs/internal/engine-decomposition.md#E-60
    // + engine-decomposition skill. Keep public getters for compat.
    std::mutex switch_mu_;
    std::string pending_symbol_;
    std::string pending_strategy_;

    std::vector<std::thread> worker_threads_;

    void pin_event_loop_thread();

    void start_workers();
    void stop_workers();

    // Centralize revocation of all [this]-capturing callbacks we installed on
    // the provider / async adapter. Called from stop paths to reduce the
    // window where in-flight callbacks can observe partially destroyed state.
    // Safe to call multiple times.
    void revoke_provider_callbacks();

    std::unique_ptr<LoggingWorker> make_logging_worker();

    // Wave 2 helpers: common skeleton for run* methods (E-40..E-44)
    // See core/docs/internal/engine-decomposition.md + engine-decomposition skill.
    // Extracted as private methods first (minimal surface on frozen file).
    // Later waves will delegate pending (W3), workers (W4).
    void clear_pending_state();
    void prepare_event_logging();
    void finalize_inline_event_log() noexcept;
    // F-07a: fail closed when a configured --instrument spec binds to no
    // symbol present in the loaded series. Runs once, before any worker.
    void validate_instrument_overrides();

    // F-08: derive the bar interval from the loaded series (the modal gap
    // between consecutive same-symbol bars). Zero when undecidable.
    void infer_bar_interval();

public:
    // F-08: the interval the decision clock is offset by, for diagnostics and
    // regression tests. Zero means "undecidable" — decision time then equals
    // the bar open, i.e. the pre-F-08 behaviour.
    std::chrono::system_clock::duration inferred_bar_interval() const
    {
        return bar_interval_;
    }

private:



    void setup_event_loop_infra();

    void teardown_event_loop_infra();
    // mid_for_symbol / drain_pending_orders moved to OrderIntentProcessor
    // (Phase 2, renamed drain_due) — call sites now say orders_->drain_due(...).
    // drain_final_pending / cancel_day_orders moved to OrderIntentProcessor
    // (Phase 3, consolidated into finalize_end_of_stream — both were always
    // called back-to-back, at all 5 call sites) — call sites now say
    // orders_->finalize_end_of_stream(...).
    // Paper maker-queue: feed a trade print into resolved adapters then drain fills.
    // No-op unless config_.maker_queue_model is set (QueueAware needs a tape).
    void feed_paper_trade_and_drain(const std::string& symbol,
                                    double price, double qty,
                                    std::optional<order_side> aggressor_side,
                                    std::chrono::system_clock::time_point ts,
                                    std::size_t& event_count, bool& halt_requested);
    // After workers join: stamp soft/data research counters onto export analytics.
    void fold_research_counters_into_export_analytics();
    void prepare_mark_prices_for_run(std::size_t symbol_hint = 8);
    void report_run_summary(std::size_t event_count, std::chrono::high_resolution_clock::time_point start_time);

public:
    engine(std::shared_ptr<data_handler> dh,
           std::shared_ptr<orderbook> ob,
           std::shared_ptr<IStrategy> strategy,
           engine_config config = {});

    ~engine();

    OrderbookRegistry& get_orderbook_registry() { return orderbook_registry_; }
    void run();
    void run_tick_data();
    void run_replay(const std::string& log_path,
                    int64_t replay_from_us = 0,
                    int64_t replay_to_us = INT64_MAX);
    StreamResult run_streaming(std::shared_ptr<DataBridge<bar_record>> bridge);
    StreamResult run_streaming(std::shared_ptr<DataBridge<tick_record>> bridge);

    // Unified-event streaming (bar/tick/l2_snapshot/l2_update). L2 events
    // populate orderbook_registry_ directly - this is how LocalBookAdapter
    // sees real exchange depth in shadow mode.
    StreamResult run_streaming(std::shared_ptr<DataBridge<provider::event>> bridge);
    bool run_succeeded() const
    {
        return !run_failed_.load(std::memory_order_acquire)
            && !halt_flag_.load(std::memory_order_acquire);
    }
    void set_strategy(std::shared_ptr<IStrategy> strategy);

    void set_primary_strategy_name(const std::string& name) { primary_strategy_name_ = name; }

    // Operator controls callable from the live TUI. Each is a single
    // atomic action; the engine checks the flags from the hot path on
    // the next event. Safe to invoke from any thread.
    // Pause/resume: when paused, the engine still drains events and
    // updates portfolio/analytics from inbound fills, but skips the
    // strategy.on_market/on_tick calls so no new orders are emitted.
    void set_pause_all(bool paused)
    {
        pause_all_.store(paused, std::memory_order_release);
    }
    bool is_pause_all() const
    {
        return pause_all_.load(std::memory_order_acquire);
    }

    // Flatten on demand: sets a one-shot flag; on the next market/tick
    // event the engine drains open positions via unwind_positions.
    // Does NOT call trigger_halt and does NOT clear halt_flag_ — halt
    // remains write-once terminal (S3); recovery is process restart only.
    void request_flatten()
    {
        flatten_request_.store(true, std::memory_order_release);
    }

    void add_strategy(std::shared_ptr<IStrategy> strategy, const std::string& name);
    void switch_symbol(const std::string& new_symbol);
    // Public API preserved exactly (external callers — tests today, operator/
    // TUI potentially later — call engine.cancel_order(...)/modify_order(...)
    // directly). Implementation moved to OrderIntentProcessor::cancel/modify
    // (Phase 3); these are now one-line forwards to orders_->cancel(...)/
    // orders_->modify(...).
    bool cancel_order(const std::string& symbol, uint64_t order_id,
                      const std::string& reason = "");
    bool modify_order(const std::string& symbol, uint64_t order_id,
                      double new_price, double new_qty);

    void apply_l2_snapshot(const std::string& symbol,
                           const std::vector<l2_level>& bids,
                           const std::vector<l2_level>& asks,
                           std::chrono::system_clock::time_point timestamp = {},
                           std::uint64_t quantity_scale = 1,
                           std::uint64_t last_update_id = 0);
    void apply_l2_update(const std::string& symbol,
                         tick_side side, double price, int64_t new_qty,
                         std::chrono::system_clock::time_point timestamp = {},
                         std::uint64_t quantity_scale = 1);
    void apply_l2_delta_batch(const provider::l2_delta_batch& batch);
    void print_summary();
    const Analytics& get_analytics() const;
    // Research honesty: invalid CSV/tick rows counted at load (before workers start).
    void set_data_rows_rejected(std::size_t n);

    // Sum of live resting quotes across paper adapters (Hybrid/QueueAware/Local).
    // Used by tests and EOS diagnostics to prove DAY cancel cleared residuals.
    std::size_t total_live_quotes() const;

    // Resets internal heavy objects (portfolio [incl. lots], analytics, exit_manager,
    // order_tracker, risk_manager, market_maker, adverse_selection, orderbook_registry,
    // shadow_tracker, attribution_ (order-attribution store), instrument/l2 caches,
    // tick aggregator, UI caches, etc.) so they can be reused for the next Monte
    // Carlo trial without full reconstruction.
    //
    // Phase 4 hardening: now clears more for per-trial isolation (attribution_,
    // shadow_tracker). Rings, workers, event_logger_, and dashboard timing are left
    // mostly untouched (workers repopulate via rings; full reset complex/unnecessary
    // for MC). See implementation comments.
    //
    // Intended for strategy/data_handler reuse experiments. MC default still
    // constructs a fresh engine per trial (MonteCarloController). This is NOT a
    // full pristine reset: execution_adapters_, pending DAY queues, and some rings
    // are left as-is until full reuse is implemented.
    //
    // Call only when you control the reuse contract (no escaped pooled events).
    void reset_for_next_trial(uint64_t new_seed);

    // Only valid in shadow mode. Returns nullptr otherwise.
    const portfolio* get_exchange_portfolio() const;
    const Analytics* get_exchange_analytics() const;

    const portfolio& get_portfolio() const { return portfolio_; }
    const truetest::exits::ExitManager& get_exit_manager() const { return exit_manager_; }

    // Fill `out` with a coherent dashboard snapshot. Returns false when no
    // snapshot exists yet (engine just constructed; first refresh hasn't
    // run). Mutex-protected; safe to call from any thread.
    bool snapshot_dashboard(truetest::ui::dashboard_snapshot& out) const;

    // Hint from the TUI (or operator actions) that the dashboard view should
    // be refreshed as soon as possible, bypassing the normal debounce.
    // Safe to call from any thread. (Fix #3)
    void request_dashboard_refresh();

    std::shared_ptr<EventRing> get_logging_ring() const { return logging_ring_; }
    std::shared_ptr<EventRing> get_risk_ring() const { return risk_ring_; }
    std::shared_ptr<EventRing> get_stats_ring() const { return stats_ring_; }
    std::shared_ptr<EventRing> get_observer_ring() const { return observer_ring_; }
    std::shared_ptr<EventRing> get_risk_stats_ring() const { return risk_stats_ring_; }

    LoggingWorker* get_logging_worker() const { return logging_worker_.get(); }
    RiskWorker* get_risk_worker() const { return risk_worker_.get(); }
    StatsWorker* get_stats_worker() const { return stats_worker_.get(); }
    ObserverWorker* get_observer_worker() const { return observer_worker_.get(); }
    RiskStatsWorker* get_risk_stats_worker() const { return risk_stats_worker_.get(); }

    const OrderTracker& get_order_tracker() const { return order_tracker_; }

    // Rejections recorded via IOrderAuditSink (Noop counts; QuestDB sink counts).
    // Used by soft-portfolio-risk tests to prove reject-only (not pass-through).
    std::size_t total_audit_rejections() const
    {
        return audit_sink_ ? audit_sink_->total_rejections() : 0;
    }

    // Halt is observable but never externally mutable. All writes must pass
    // through trigger_halt so terminal state, wakeup, and diagnostics agree.
    bool is_halted() const
    {
        return halt_flag_.load(std::memory_order_acquire);
    }
    const std::atomic<bool>& get_halt_flag() const { return halt_flag_; }

    // Single thread-safe halt entry-point. Use this everywhere a halt is
    // raised (ring drop, watchdog hang, network detector, operator action)
    // so the dashboard banner, halt_flag_, and the recent-events ring stay
    // in sync. Idempotent: only the first caller per run wins, the rest
    // are no-ops. `reason` is truncated to streaming_stats::shutdown_reason_cap.
    void trigger_halt(std::string_view reason) noexcept override;

    // Terminal operator action. The shared live session performs one
    // kill-before-close sequence and returns the cached result to all callers.
    bool request_operator_kill(std::chrono::milliseconds deadline);
    bool finalize_live_shutdown(live_shutdown_reason reason);
};
