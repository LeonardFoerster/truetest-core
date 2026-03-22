#pragma once
#include "../data/data_handler.h"
#include "../strategy/strategy_interface.h"
#include "../execution/portfolio.h"
#include "../execution/execution_adapter.h"
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

// stage_timer.h included unconditionally — provides DEBUG_STAGE macro
// that compiles to ((void)0) when HAS_DEBUG is off
#include "../debug/stage_timer.h"

#ifdef HAS_DEBUG
#include "../debug/debug_log.h"
#include "../debug/memory_info.h"
#include "../debug/ring_stats.h"
#include "../debug/debug_report.h"
#endif

#include <atomic>
#include <memory>
#include <queue>
#include <thread>

// Default ring buffer size for outbound worker channels
static constexpr std::size_t DEFAULT_RING_SIZE = 65536;

using EventRing = RingBuffer<event_pointer, DEFAULT_RING_SIZE>;

class engine
{
private:
    engine_config config_;
    std::shared_ptr<data_handler> data_handler_;
    OrderbookRegistry orderbook_registry_;
    std::shared_ptr<IStrategy> strategy_;
    // Per-symbol execution adapters (created on demand)
    std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>> execution_adapters_;
    portfolio portfolio_;
    Analytics analytics_;
    RiskManager risk_manager_;
    MarketMaker market_maker_;
    double last_mid_price_ = 0.0;

    // Object pools for hot-path event allocation (avoid heap pressure)
    ObjectPool<market_event> market_pool_;
    ObjectPool<order_event>  order_pool_;
    ObjectPool<fill_event>   fill_pool_;
    ObjectPool<tick_event>   tick_pool_;

    // Outbound ring buffers (only the ones used by the active preset are non-null)
    std::shared_ptr<EventRing> logging_ring_;       // standard, full, extended
    std::shared_ptr<EventRing> risk_ring_;           // full, extended
    std::shared_ptr<EventRing> stats_ring_;          // full, extended
    std::shared_ptr<EventRing> observer_ring_;       // light
    std::shared_ptr<EventRing> risk_stats_ring_;     // standard
    std::shared_ptr<EventRing> mm_ring_;             // extended (outbound to MM)
    std::shared_ptr<MMRing> mm_order_ring_;          // extended (inbound from MM)

    // Publish an event to all outbound rings (no-op when threading disabled)
    void publish_event(const event_pointer& ev);

    // Optional event logger (created when event_log_path is set)
    std::unique_ptr<EventLogger> event_logger_;

    // Log an event to the event log (no-op when logger is null)
    void log_event(const event& ev);

    // Get or create execution adapter for a symbol
    std::shared_ptr<IExecutionAdapter> get_adapter(const std::string& symbol);

    // Threading: shared halt flag (risk worker → engine loop)
    std::atomic<bool> halt_flag_{false};

    // Worker failure flag (any worker exception → engine loop stops)
    std::atomic<bool> worker_failed_{false};

    // Ring buffer drop counters
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

    // Worker instances (only the ones used by the active preset are non-null)
    std::unique_ptr<LoggingWorker> logging_worker_;
    std::unique_ptr<RiskWorker> risk_worker_;
    std::unique_ptr<StatsWorker> stats_worker_;
    std::unique_ptr<ObserverWorker> observer_worker_;
    std::unique_ptr<RiskStatsWorker> risk_stats_worker_;
    std::unique_ptr<MarketMakerWorker> mm_worker_;

    // Worker threads
    std::vector<std::thread> worker_threads_;

    // Start/stop worker threads
    void start_workers();
    void stop_workers();

    // Create logging worker with config-based sink selection
    std::unique_ptr<LoggingWorker> make_logging_worker();

public:
    // Construct with a pre-built orderbook (backward compatible, single-symbol)
    engine(std::shared_ptr<data_handler> dh,
           std::shared_ptr<orderbook> ob,
           std::shared_ptr<IStrategy> strategy,
           engine_config config = {});

    // Access the orderbook registry
    OrderbookRegistry& get_orderbook_registry() { return orderbook_registry_; }
    void run();
    void run_tick_data();
    void run_replay(const std::string& log_path);
    void print_summary();

    // Access outbound rings for wiring worker threads
    std::shared_ptr<EventRing> get_logging_ring() const { return logging_ring_; }
    std::shared_ptr<EventRing> get_risk_ring() const { return risk_ring_; }
    std::shared_ptr<EventRing> get_stats_ring() const { return stats_ring_; }
    std::shared_ptr<EventRing> get_observer_ring() const { return observer_ring_; }
    std::shared_ptr<EventRing> get_risk_stats_ring() const { return risk_stats_ring_; }

    // Access workers for testing / monitoring
    LoggingWorker* get_logging_worker() const { return logging_worker_.get(); }
    RiskWorker* get_risk_worker() const { return risk_worker_.get(); }
    StatsWorker* get_stats_worker() const { return stats_worker_.get(); }
    ObserverWorker* get_observer_worker() const { return observer_worker_.get(); }
    RiskStatsWorker* get_risk_stats_worker() const { return risk_stats_worker_.get(); }

    // Halt flag for external injection (testing)
    std::atomic<bool>& get_halt_flag() { return halt_flag_; }
};
