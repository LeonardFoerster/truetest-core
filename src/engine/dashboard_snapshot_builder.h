#pragma once

#include "ui/dashboard_snapshot.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/event.h"  // order_event, fill_event
#include "execution/mark_point.h"  // R3: marks carry observation timestamps
#include "execution/order_tracker.h"  // R3: authoritative order ledger
#include "risk/risk_manager.h"        // R3: daily realized loss for the risk panel

// Cold-path includes for snapshot builder (no hot path impact).
#include "types/object_pool.h"
#include "threading/ring_buffer.h"
#include "types/control_block_pool.h"

// Strongly-typed debug samplers to eliminate void* miswiring risk in the
// large injection ctor (see MEDIUM-01 in memory safety checks).
// These are only meaningful under HAS_DEBUG; otherwise both are null.
namespace debug {
    class StageTimer;
    class MemorySampler;
}
struct DebugSamplers {
    debug::StageTimer*    stage_timer   = nullptr;
    debug::MemorySampler* memory_sampler = nullptr;
};

// Forward declarations to keep includes minimal in header (cold path).
class portfolio;
class Analytics;
class AdverseSelectionTracker;
namespace truetest { namespace exits { class ExitManager; } }
class OrderbookRegistry;
class IOrderAuditSink;
class PendingOrderScheduler;
class OrderAttributionStore;
struct engine_config;
class IExecutionAdapter;

// Pools and rings for debug snapshot (cold) - defined by includes.
using EventRing = RingBuffer<event_pointer, 65536>;

class DashboardSnapshotBuilder
{
public:
    // Ctor injects all data sources needed for snapshot construction and cache
    // mutation. All non-owning refs. Builder is cold-path only.
    //
    // Lifetime contract: every injected reference (pools, rings, portfolio,
    // analytics, exit_manager, config, registry, the audit-sink owner slot,
    // pending scheduler, attribution store, last_* refs, execution_adapters
    // map, debug samplers, control block pool) MUST outlive this builder. The
    // engine guarantees this by owning all of them for the lifetime of the
    // builder (which is a unique_ptr member of engine). Reference the owning
    // audit-sink slot, not its replaceable pointee.
    // Rings are empty at ctor time and populated in start_workers(); because
    // we hold references to the engine's shared_ptr< EventRing > members, later
    // assignments become visible (post-ctor "mutation" of the ring targets).
    // MC reuse: call clear_for_mc_reset() + pool rearm; do not let external
    // snapshot holders or escaped event_pointers from prior epoch dangle across
    // reset (epoch guards in pools + armed callbacks mitigate).
    // See 2026-07-18 memory-check HIGH-03.
    explicit DashboardSnapshotBuilder(
        const portfolio& port,
        // R3: authoritative order ledger. The operator surfaces read their
        // order counts from it instead of deriving them from cached rows plus
        // fill counters.
        const OrderTracker& order_tracker,
        const RiskManager& risk_manager,
        const Analytics& analytics,
        const AdverseSelectionTracker& adverse,
        const truetest::exits::ExitManager& exits,
        const std::atomic<bool>& halt_flag,
        const engine_config& config,
        // Atomic<double> ref instead of plain double& to avoid swappy aliasing
        // and data-race surface on concurrent price updates from event loop.
        // Snapshot code will .load() as needed.
        const std::atomic<double>& last_mid_price,
        const std::string& last_mark_symbol,
        // Per-symbol marks (FR-06); same map engine uses for final/shadow equity.
        // Lifetime: engine-owned; written on event thread, read on cold snapshot
        // (e.g. the web server's poller thread, a genuine concurrent reader —
        // see web/web_server.h). Unlike last_mid_price_ this is a plain
        // unordered_map, so every access must go through last_mark_prices_mu.
        const std::unordered_map<std::string, mark_point>& last_mark_prices,
        std::mutex& last_mark_prices_mu,
        OrderbookRegistry& orderbook_registry,
        const std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>>& execution_adapters,
        const std::shared_ptr<IOrderAuditSink>& audit_sink,
        const PendingOrderScheduler& pending_scheduler,
        const OrderAttributionStore& attribution,
        const std::unordered_set<std::string>& l2_seeded_symbols,
        // Pools for memory/debug stats (cold)
        const ObjectPool<market_event>& market_pool,
        const ObjectPool<order_event>& order_pool,
        const ObjectPool<fill_event>& fill_pool,
        const ObjectPool<tick_event>& tick_pool,
        const ObjectPool<l2_update_event>& l2_update_pool,
        const ObjectPool<l2_snapshot_event>& l2_snapshot_pool,
        const ObjectPool<rejection_event>& rejection_pool,
        const ObjectPool<cancel_event>& cancel_pool,
        const ObjectPool<amend_event>& amend_pool,
        const ObjectPool<funding_event>& funding_pool,
        const ControlBlockPool& control_block_pool,
        // Rings for debug (cold)
        const std::shared_ptr<EventRing>& logging_ring,
        const std::shared_ptr<EventRing>& risk_ring,
        const std::shared_ptr<EventRing>& stats_ring,
        const std::shared_ptr<EventRing>& observer_ring,
        const std::shared_ptr<EventRing>& risk_stats_ring,
        const std::shared_ptr<EventRing>& mm_ring,
        // For debug stages and memory sampler if HAS_DEBUG.
        // Strongly typed aggregate eliminates adjacent-void* swappability UB.
        DebugSamplers debug_samplers = {}
    );

    ~DashboardSnapshotBuilder();

    // Public API delegated from engine (behavior identical).
    bool snapshot_dashboard(truetest::ui::dashboard_snapshot& out) const;
    void request_dashboard_refresh();

    // Called from publish_event (cold tick).
    void refresh_if_due();

    // Cache mutations called from hot paths (order processing, fills etc.).
    // These stay allocation-light on the event loop.
    void cache_open_order(const order_event& o);
    void update_open_order_status(std::uint64_t id, const char* status);
    void erase_open_order(std::uint64_t id);
    void cache_fill(const fill_event& f);

    // For MC trial reuse (E-36).
    void clear_for_mc_reset();

private:
    // Moved state (was in engine)
    mutable std::mutex                    dashboard_view_mu_;
    truetest::ui::dashboard_snapshot      dashboard_view_;
    bool                                  dashboard_view_initialised_ = false;
    std::chrono::steady_clock::time_point dashboard_view_last_{};
    bool                                  dashboard_view_force_ = false;

    mutable truetest::ui::dashboard_snapshot::memory_view memory_cache_{};
    mutable std::chrono::steady_clock::time_point         memory_cache_last_{};
    mutable bool                                          memory_cache_initialised_ = false;
    std::chrono::milliseconds             dashboard_view_interval_{100};

    struct open_order_cache_entry
    {
        truetest::ui::dashboard_snapshot::open_order_row row{};
        std::chrono::system_clock::time_point            ts{};
    };
    std::unordered_map<std::uint64_t, open_order_cache_entry> open_orders_cache_;
    std::deque<truetest::ui::dashboard_snapshot::fill_row>    recent_fills_cache_;
    static constexpr std::size_t kRecentFillsCap = 64;

    void build_dashboard_view(truetest::ui::dashboard_snapshot& out) const;

    // Injected data sources (non-owning)
    const portfolio& portfolio_;
    const OrderTracker& order_tracker_;
    const RiskManager& risk_manager_;
    const Analytics& analytics_;
    const AdverseSelectionTracker& adverse_selection_;
    const truetest::exits::ExitManager& exit_manager_;
    const std::atomic<bool>& halt_flag_;
    const engine_config& config_;
    const std::atomic<double>& last_mid_price_;
    const std::string& last_mark_symbol_;
    const std::unordered_map<std::string, mark_point>& last_mark_prices_;
    std::mutex& last_mark_prices_mu_;
    OrderbookRegistry& orderbook_registry_;
    const std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>>& execution_adapters_;
    const std::shared_ptr<IOrderAuditSink>& audit_sink_;
    const PendingOrderScheduler& pending_scheduler_;
    const OrderAttributionStore& attribution_;
    const std::unordered_set<std::string>& l2_seeded_symbols_;

    // Pools (for memory/debug stats)
    const ObjectPool<market_event>& market_pool_;
    const ObjectPool<order_event>& order_pool_;
    const ObjectPool<fill_event>& fill_pool_;
    const ObjectPool<tick_event>& tick_pool_;
    const ObjectPool<l2_update_event>& l2_update_pool_;
    const ObjectPool<l2_snapshot_event>& l2_snapshot_pool_;
    const ObjectPool<rejection_event>& rejection_pool_;
    const ObjectPool<cancel_event>& cancel_pool_;
    const ObjectPool<amend_event>& amend_pool_;
    const ObjectPool<funding_event>& funding_pool_;
    const ControlBlockPool& control_block_pool_;

    // Rings
    const std::shared_ptr<EventRing>& logging_ring_;
    const std::shared_ptr<EventRing>& risk_ring_;
    const std::shared_ptr<EventRing>& stats_ring_;
    const std::shared_ptr<EventRing>& observer_ring_;
    const std::shared_ptr<EventRing>& risk_stats_ring_;
    const std::shared_ptr<EventRing>& mm_ring_;

    // Debug (optional, may be null)
    DebugSamplers debug_samplers_;
};
