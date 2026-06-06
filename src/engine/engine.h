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
#include "types/object_pool.h"
#include "exits/exit_manager.h"

#include "debug/stage_timer.h"

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
#include "providers/local/csv_parser.h"
#include "providers/provider_event.h"
#include "analytics/shadow_tracker.h"
#include "strategy/strategy_factory.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>

static constexpr std::size_t DEFAULT_RING_SIZE = 65536;

using EventRing = RingBuffer<event_pointer, DEFAULT_RING_SIZE>;

class engine
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
    // the provider returns at least one liveness_source — currently
    // only the futures dead-man's switch heartbeat opts in.
    std::unique_ptr<WorkerWatchdog> worker_watchdog_;
    MarketMaker market_maker_;
    double last_mid_price_ = 0.0;
    std::string last_mark_symbol_;

    std::unordered_map<std::string, std::optional<instrument_spec>> instrument_cache_;

    // Symbols already carrying real L2 depth — MarketMaker::replenish is
    // suppressed here so paper liquidity can't corrupt the fill sim.
    std::unordered_set<std::string> l2_seeded_symbols_;
    const instrument_spec* resolve_instrument_spec(const std::string& symbol);
    bool apply_instrument_spec(order_event& o, const instrument_spec& spec) const;

    std::unique_ptr<BarAggregator> tick_aggregator_;
    std::chrono::milliseconds tick_bar_interval_{60000};

    ObjectPool<market_event> market_pool_;
    ObjectPool<order_event>  order_pool_;
    ObjectPool<fill_event>   fill_pool_;
    ObjectPool<tick_event>   tick_pool_;

    std::shared_ptr<EventRing> logging_ring_;
    std::shared_ptr<EventRing> risk_ring_;
    std::shared_ptr<EventRing> stats_ring_;
    std::shared_ptr<EventRing> observer_ring_;
    std::shared_ptr<EventRing> risk_stats_ring_;
    std::shared_ptr<EventRing> mm_ring_;
    std::shared_ptr<MMRing> mm_order_ring_;

    void publish_event(const event_pointer& ev);

#ifdef HAS_QUESTDB
    std::shared_ptr<truetest::questdb::QuestdbStore> questdb_store_;
    bool questdb_active_ = false;  // true only after successful begin()
    std::size_t questdb_total_rejections_ = 0;

    // Last time we called tick() for time-based ILP flushing (Phase 1 hardening).
    std::chrono::steady_clock::time_point last_questdb_flush_{};

    void questdb_begin();
    void questdb_end();

    // Cheap periodic call (intended to be invoked from the 200ms reporting blocks).
    // Does nothing if persist is not active. Calls QuestdbStore::tick() at most
    // once per config_.questdb_flush_cadence.
    void maybe_questdb_tick();
#endif

    // Dashboard view: read from the rich (ncurses) TUI render thread.
    // Filled on the event loop (no contention with the hot path) and
    // swapped into the slot under a mutex; readers take a quick lock to
    // copy. Refresh is debounced to ~100 ms aligned with the render tick.
    mutable std::mutex                    dashboard_view_mu_;
    truetest::ui::dashboard_snapshot      dashboard_view_;
    bool                                  dashboard_view_initialised_ = false;
    std::chrono::steady_clock::time_point dashboard_view_last_{};
    bool                                  dashboard_view_force_ = false;  // set by request_dashboard_refresh (Fix #3)

    // Memory-view cache. /proc/self/* parsing was the dominant cost of
    // the snapshot path; refresh at ~1 Hz instead of every snapshot.
    // mutable so build_dashboard_view (const) can update it.
    mutable truetest::ui::dashboard_snapshot::memory_view memory_cache_{};
    mutable std::chrono::steady_clock::time_point         memory_cache_last_{};
    mutable bool                                          memory_cache_initialised_ = false;
    std::chrono::milliseconds             dashboard_view_interval_{100};
    void refresh_dashboard_view_if_due();
    void build_dashboard_view(truetest::ui::dashboard_snapshot& out) const;

    // Side caches for the rich TUI's Orders & Fills pane. Mutated on the
    // event-loop thread alongside order_tracker_/portfolio_; copied into
    // the snapshot during build_dashboard_view().
    struct open_order_cache_entry
    {
        truetest::ui::dashboard_snapshot::open_order_row row{};
        std::chrono::system_clock::time_point            ts{};
    };
    std::unordered_map<std::uint64_t, open_order_cache_entry> open_orders_cache_;
    std::deque<truetest::ui::dashboard_snapshot::fill_row>    recent_fills_cache_;
    static constexpr std::size_t kRecentFillsCap = 64;

    void cache_open_order(const order_event& o);
    void update_open_order_status(std::uint64_t id, const char* status);
    void erase_open_order(std::uint64_t id);
    void cache_fill(const fill_event& f);

    void write_checkpoint_if_due(std::size_t event_count);
    void restore_from_checkpoint();

    std::unique_ptr<EventLogger> event_logger_;

    std::unique_ptr<ShadowTracker> shadow_tracker_;

    truetest::exits::ExitManager exit_manager_;

    void register_strategy_exit_intent(IStrategy& strategy,
                                       const std::string& strategy_name,
                                       std::uint64_t order_id);

    // Invoked by the engine on each fill-poll cycle to register any
    // venue-bracket-leg metadata produced by the unknown_fill_handler
    // installed on the provider's ExecutionBridge. Safe to call when
    // there is no bridge — it just no-ops.
    void drain_venue_bracket_meta();

    // Stamp per-lot attribution (opener_order_id + strategy_name) onto a
    // fill_event if not already present. Uses order_meta_ lookup as fallback.
    // Called from all fill processing paths (and adapters now set it at
    // creation for paper/shadow fills). Part of Phase 1 deepdive per-lot
    // consolidation.
    void stamp_fill_attribution(fill_event& f);

    // Returns true if an exit fire caused the engine to halt.
    bool evaluate_exits(const std::string& symbol, double px,
                        std::chrono::system_clock::time_point ts,
                        std::size_t& event_count,
                        std::int64_t recv_ns);

    // Bar variant: probes the bar's low/high so an intra-bar wick through
    // SL/TP fires the bracket. Tick paths keep the price-only overload.
    bool evaluate_exits(const std::string& symbol,
                        double low, double high, double close,
                        std::chrono::system_clock::time_point ts,
                        std::size_t& event_count,
                        std::int64_t recv_ns);

    void log_event(const event& ev);

    std::shared_ptr<IExecutionAdapter> get_adapter(const std::string& symbol);

    bool process_order(const std::shared_ptr<order_event>& o,
                       std::size_t& event_count,
                       bool& halt_requested);

    void unwind_positions(std::size_t& event_count);

    bool route_order(order_event& order,
                     const std::chrono::system_clock::time_point& sim_time,
                     std::size_t& event_count, bool& halt_requested);

    void check_pending_stops(double high, double low,
                             const std::chrono::system_clock::time_point& sim_time,
                             std::size_t& event_count, bool& halt_requested);

    void dispatch_extras_on_market(const market_event& mkt,
                                   const std::chrono::system_clock::time_point& ts,
                                   std::size_t& event_count);
    void dispatch_extras_on_tick(const tick_event& te,
                                 const std::chrono::system_clock::time_point& ts,
                                 std::size_t& event_count);
    void notify_position_change_all(const std::string& symbol, bool open);

    void process_single_bar(const bar_record& rec, std::size_t& event_count,
                            const std::chrono::system_clock::time_point& timestamp);

    void process_single_tick(const tick_record& rec, std::size_t& event_count);

    std::vector<std::shared_ptr<order_event>> pending_stops_;

    struct pending_entry
    {
        std::shared_ptr<order_event> order;
        uint64_t seq;
    };
    static bool pending_cmp(const pending_entry& a, const pending_entry& b)
    {
        if (a.order->get_earliest_eligible_ts() != b.order->get_earliest_eligible_ts())
            return a.order->get_earliest_eligible_ts() > b.order->get_earliest_eligible_ts();
        return a.seq > b.seq;
    }
    std::priority_queue<pending_entry, std::vector<pending_entry>,
                        decltype(&engine::pending_cmp)> pending_orders_{&engine::pending_cmp};
    uint64_t order_seq_ = 0;

    std::vector<std::pair<std::string, uint64_t>> day_order_ids_;

    // Per-order metadata recorded at route time. Used when fills come back
    // to route them to the right lot (opener_order_id) and to tag the lot
    // with its owning strategy.
    struct order_meta
    {
        uint64_t opener_order_id = 0;
        std::string strategy_name;
    };
    std::unordered_map<uint64_t, order_meta> order_meta_;

    void register_order_meta(const order_event& o);
    uint64_t lookup_opener(uint64_t order_id) const;
    const std::string& lookup_strategy_name(uint64_t order_id) const;

    // Routes a fill back to the strategy that emitted the originating
    // order, by matching strategy_name against primary/additional sets.
    void dispatch_fill_to_strategy(const fill_event& f);

    std::atomic<bool> halt_flag_{false};
    std::atomic<bool> worker_failed_{false};
    std::atomic<bool> pause_all_{false};
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

    std::mutex switch_mu_;
    std::string pending_symbol_;
    std::string pending_strategy_;

    std::vector<std::thread> worker_threads_;

    void pin_event_loop_thread();

    void start_workers();
    void stop_workers();

    std::unique_ptr<LoggingWorker> make_logging_worker();

public:
    engine(std::shared_ptr<data_handler> dh,
           std::shared_ptr<orderbook> ob,
           std::shared_ptr<IStrategy> strategy,
           engine_config config = {});

    OrderbookRegistry& get_orderbook_registry() { return orderbook_registry_; }
    void run();
    void run_tick_data();
    void run_replay(const std::string& log_path,
                    int64_t replay_from_us = 0,
                    int64_t replay_to_us = INT64_MAX);
    void run_streaming(std::shared_ptr<DataBridge<bar_record>> bridge);
    void run_streaming(std::shared_ptr<DataBridge<tick_record>> bridge);

    // Unified-event streaming (bar/tick/l2_snapshot/l2_update). L2 events
    // populate orderbook_registry_ directly — this is how LocalBookAdapter
    // sees real exchange depth in shadow mode.
    void run_streaming(std::shared_ptr<DataBridge<provider::event>> bridge);
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

    // Flatten on demand: drains all open positions through the unwind
    // path. Halts the engine afterwards (operator can resume by clearing
    // the halt flag separately if desired).
    void request_flatten()
    {
        flatten_request_.store(true, std::memory_order_release);
    }

    void add_strategy(std::shared_ptr<IStrategy> strategy, const std::string& name)
    {
        if (!strategy) return;
        additional_strategies_.push_back(std::move(strategy));
        additional_strategy_names_.push_back(name);
    }
    void switch_symbol(const std::string& new_symbol);
    bool cancel_order(const std::string& symbol, uint64_t order_id,
                      const std::string& reason = "");
    bool modify_order(const std::string& symbol, uint64_t order_id,
                      double new_price, double new_qty);

    void apply_l2_snapshot(const std::string& symbol,
                           const std::vector<l2_level>& bids,
                           const std::vector<l2_level>& asks);
    void apply_l2_update(const std::string& symbol,
                         tick_side side, double price, int64_t new_qty);
    void print_summary();
    const Analytics& get_analytics() const;

    // Resets internal heavy objects (portfolio [incl. lots], analytics, exit_manager,
    // order_tracker, risk_manager, market_maker, adverse_selection, orderbook_registry,
    // shadow_tracker, order_meta_, instrument/l2 caches, tick aggregator, UI caches, etc.)
    // so they can be reused for the next Monte Carlo trial without full reconstruction.
    //
    // Phase 4 hardening: now clears more for per-trial isolation (order_meta_,
    // shadow_tracker). Rings, workers, event_logger_, and dashboard timing are left
    // mostly untouched (workers repopulate via rings; full reset complex/unnecessary
    // for MC). See implementation comments.
    //
    // This is intended primarily for MonteCarloController when reuse_objects_between_trials
    // is enabled. It is NOT a general-purpose reset and does not restore the engine to a
    // pristine post-construction state in all cases.
    //
    // Call this after engine construction (or between trials) when you want to reuse the
    // engine instance across multiple independent backtests.
    void reset_for_next_trial(uint64_t new_seed);

    // Only valid in shadow mode. Returns nullptr otherwise.
    const portfolio* get_exchange_portfolio() const;
    const Analytics* get_exchange_analytics() const;

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

    std::atomic<bool>& get_halt_flag() { return halt_flag_; }

    // Single thread-safe halt entry-point. Use this everywhere a halt is
    // raised (ring drop, watchdog hang, network detector, operator action)
    // so the dashboard banner, halt_flag_, and the recent-events ring stay
    // in sync. Idempotent: only the first caller per run wins, the rest
    // are no-ops. `reason` is truncated to streaming_stats::shutdown_reason_cap.
    void trigger_halt(std::string_view reason);
};
