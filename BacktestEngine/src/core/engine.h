#pragma once
#include <climits>
#include <cstdint>
#include "../data/data_handler.h"
#include "../strategy/strategy_interface.h"
#include "../execution/portfolio.h"
#include "../execution/execution_adapter.h"
#include "../execution/order_tracker.h"
#include "../orderbook/orderbook.h"
#include "../orderbook/orderbook_registry.h"
#include "../market_maker/market_maker.h"
#include "../analytics/analytics.h"
#include "../analytics/bar_aggregator.h"
#include "../risk/risk_manager.h"
#include "../threading/ring_buffer.h"
#include "../threading/thread_config.h"
#include "../threading/logging_worker.h"
#include "../threading/risk_worker.h"
#include "../threading/stats_worker.h"
#include "../threading/observer_worker.h"
#include "../threading/risk_stats_worker.h"
#include "../threading/market_maker_worker.h"
#include "engine_config.h"
#include "event.h"
#include "event_log.h"
#include "../types/order_id.h"
#include "../types/object_pool.h"

#include "../debug/stage_timer.h"

#ifdef HAS_SQLITE
#include "../data/sqlite_store.h"
#endif

#ifdef HAS_WEB_UI
#include "../threading/ws_worker.h"
#endif

#ifdef HAS_DEBUG
#include "../debug/debug_log.h"
#include "../debug/memory_info.h"
#include "../debug/ring_stats.h"
#include "../debug/debug_report.h"
#endif

#include "../providers/data_bridge.h"
#include "../providers/local/csv_parser.h"
#include "../analytics/shadow_tracker.h"
#include "../strategy/strategy_factory.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

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
    OrderTracker order_tracker_;
    Analytics analytics_;
    RiskManager risk_manager_;
    MarketMaker market_maker_;
    double last_mid_price_ = 0.0;

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

#ifdef HAS_SQLITE
    std::unique_ptr<SqliteStore> store_;
    std::string current_run_id_;
    void record_run_begin();
    void record_run_end();
#endif

    void write_checkpoint_if_due(std::size_t event_count);
    void restore_from_checkpoint();

    std::unique_ptr<EventLogger> event_logger_;

    std::unique_ptr<ShadowTracker> shadow_tracker_;

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

    std::atomic<bool> halt_flag_{false};
    std::atomic<bool> worker_failed_{false};

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

#ifdef HAS_WEB_UI
    std::shared_ptr<EventRing> ws_ring_;
    std::unique_ptr<WebSocketWorker> ws_worker_;
    std::size_t ws_drops_ = 0;

    void process_ws_commands(bool& halt_requested, std::size_t& event_count);
    void broadcast_orderbook_snapshot(const std::string& symbol);
    void send_state_snapshot();
    void broadcast_market_with_indicators(const market_event& mkt);

    std::chrono::steady_clock::time_point last_ob_snapshot_time_;

    static constexpr std::size_t MAX_BAR_HISTORY = 1000;
    std::vector<std::string> bar_history_;
#endif

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
    void set_strategy(std::shared_ptr<IStrategy> strategy);

    void set_primary_strategy_name(const std::string& name) { primary_strategy_name_ = name; }

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
};
