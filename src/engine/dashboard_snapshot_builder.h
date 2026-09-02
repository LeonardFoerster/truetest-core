#pragma once

#include "dashboard_projection.h"
#include "ui/dashboard_snapshot.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/event.h"  // order_event, fill_event

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

struct DashboardEngineDebugSampler
{
    const void* context = nullptr;
    DashboardEngineDebugCounts (*sample)(const void*) noexcept = nullptr;

    DashboardEngineDebugCounts operator()() const noexcept
    {
        return sample ? sample(context) : DashboardEngineDebugCounts{};
    }

    explicit operator bool() const noexcept { return sample != nullptr; }
};

// Forward declarations to keep includes minimal in header (cold path).
class portfolio;
class Analytics;
class AdverseSelectionTracker;
namespace truetest { namespace exits { class ExitManager; } }
class OrderbookRegistry;
struct engine_config;
class IExecutionAdapter;

// Pools and rings for debug snapshot (cold) - defined by includes.
using EventRing = RingBuffer<event_pointer, 65536>;

class DashboardSnapshotBuilder
{
public:
    // Ctor injects all data sources needed for fixed projection capture,
    // cold snapshot materialization, and cache mutation. All refs are
    // non-owning. Producer capture is bounded/noexcept; rich construction is
    // reader-side cold work.
    //
    // Lifetime contract: every injected reference (pools, rings, portfolio,
    // analytics, exit_manager, config, registry, last_* refs,
    // execution_adapters map, debug samplers, control block pool) MUST outlive
    // this builder. The engine guarantees this by owning all of them for the
    // lifetime of the builder (which is a unique_ptr member of engine).
    // Rings are empty at ctor time and populated in start_workers(); because
    // we hold references to the engine's shared_ptr< EventRing > members, later
    // assignments become visible (post-ctor "mutation" of the ring targets).
    // MC reuse: call clear_for_mc_reset() + pool rearm; do not let external
    // snapshot holders or escaped event_pointers from prior epoch dangle across
    // reset (epoch guards in pools + armed callbacks mitigate).
    // See 2026-07-18 memory-check HIGH-03.
    explicit DashboardSnapshotBuilder(
        const portfolio& port,
        const Analytics& analytics,
        const AdverseSelectionTracker& adverse,
        const truetest::exits::ExitManager& exits,
        const std::atomic<bool>& halt_flag,
        const std::atomic<std::size_t>& active_order_count,
        const engine_config& config,
        // Atomic<double> ref instead of plain double& to avoid swappy aliasing
        // and data-race surface on concurrent price updates from event loop.
        // Snapshot code will .load() as needed.
        const std::atomic<double>& last_mid_price,
        const std::string& last_mark_symbol,
        // Per-symbol marks (FR-06). They are read only by the sole event
        // producer while creating a projection; UI/web readers never touch it.
        const std::unordered_map<std::string, double>& last_mark_prices,
        OrderbookRegistry& orderbook_registry,
        const std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>>& execution_adapters,
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
        DashboardEngineDebugSampler engine_debug_counts,
        // For debug stages and memory sampler if HAS_DEBUG.
        // Strongly typed aggregate eliminates adjacent-void* swappability UB.
        DebugSamplers debug_samplers = {}
    );

    ~DashboardSnapshotBuilder();

    // Public API delegated from engine (behavior identical).
    bool snapshot_dashboard(truetest::ui::dashboard_snapshot& out) const;
    void request_dashboard_refresh();

    // Event-boundary protocol. Calls may nest; only the outermost normally
    // completed boundary is eligible to capture. No capture occurs while an
    // exception is unwinding.
    void begin_event_boundary() noexcept;
    void end_event_boundary(bool normally_completed) noexcept;

    // Sole-producer projection publication. These functions are allocation-
    // free, lock-free and noexcept after construction.
    bool refresh_if_due() noexcept;
    bool publish_initial_snapshot() noexcept;
    bool publish_final_snapshot() noexcept;

    // Cache mutations called from hot paths (order processing, fills etc.).
    // These stay allocation-light on the event loop.
    void cache_open_order(const order_event& o) noexcept;
    void update_open_order_status(std::uint64_t id,
                                  const char* status) noexcept;
    void erase_open_order(std::uint64_t id) noexcept;
    void cache_fill(const fill_event& f) noexcept;

    // For MC trial reuse (E-36).
    void clear_for_mc_reset();

private:
    friend class DashboardProjectionTestPeer;
    static constexpr std::uint8_t kSnapshotSlotCount = 3;
    static constexpr std::uint8_t kNoPublishedSnapshot = kSnapshotSlotCount;
    static constexpr std::uint32_t kSnapshotWriterOwned = 1U << 31U;
    static constexpr std::uint32_t kSnapshotReaderMask =
        kSnapshotWriterOwned - 1U;
    static constexpr std::uint64_t kNoPublishedSnapshotToken =
        ~std::uint64_t{0};
    struct snapshot_slot
    {
        truetest::dashboard::DashboardProjection value;
        // One state word closes the load-before-pin race: readers increment
        // only while the writer bit is clear, and a writer can claim a slot
        // only from the exact zero state.
        mutable std::atomic<std::uint32_t> access_state{0};
    };

    class projection_pin
    {
    public:
        projection_pin(const projection_pin&) = delete;
        projection_pin& operator=(const projection_pin&) = delete;

        projection_pin(projection_pin&& other) noexcept
            : state_(std::exchange(other.state_, nullptr))
            , value_(std::exchange(other.value_, nullptr))
        {}

        projection_pin& operator=(projection_pin&& other) noexcept
        {
            if (this == &other) return *this;
            release();
            state_ = std::exchange(other.state_, nullptr);
            value_ = std::exchange(other.value_, nullptr);
            return *this;
        }

        ~projection_pin() { release(); }

        const truetest::dashboard::DashboardProjection& value() const noexcept
        {
            return *value_;
        }

    private:
        friend class DashboardSnapshotBuilder;
        projection_pin(
            std::atomic<std::uint32_t>& state,
            const truetest::dashboard::DashboardProjection& value) noexcept
            : state_(&state)
            , value_(&value)
        {}

        void release() noexcept
        {
            if (!state_) return;
            state_->fetch_sub(1U, std::memory_order_release);
            state_ = nullptr;
            value_ = nullptr;
        }

        std::atomic<std::uint32_t>* state_ = nullptr;
        const truetest::dashboard::DashboardProjection* value_ = nullptr;
    };

    // Single engine-thread producer, bounded non-blocking publication. A
    // reader pins and validates the published slot before copying; the writer
    // skips an observational refresh when both inactive slots are pinned.
    mutable std::array<snapshot_slot, kSnapshotSlotCount> snapshot_slots_{};
    mutable std::atomic<std::uint64_t> published_snapshot_token_{
        kNoPublishedSnapshotToken};
    std::uint64_t snapshot_generation_ = 0;
    std::chrono::steady_clock::time_point dashboard_view_last_{};
    mutable std::atomic<std::uint64_t> dashboard_request_epoch_{1};
    mutable std::atomic<std::uint64_t> dashboard_force_epoch_{1};
    std::uint64_t dashboard_captured_epoch_ = 0;
    std::size_t event_boundary_depth_ = 0;
    bool event_boundary_failed_ = false;

    mutable std::mutex                                    memory_cache_mu_;
    mutable truetest::ui::dashboard_snapshot::memory_view memory_cache_{};
    mutable std::chrono::steady_clock::time_point         memory_cache_last_{};
    mutable bool                                          memory_cache_initialised_ = false;
    std::chrono::milliseconds             dashboard_view_interval_{100};

    struct open_order_cache_entry
    {
        enum class slot_state : std::uint8_t { empty, occupied, tombstone };
        slot_state state = slot_state::empty;
        truetest::ui::dashboard_snapshot::open_order_row row{};
        std::chrono::system_clock::time_point            ts{};
    };
    // Allocated and string-reserved during engine construction. The table is
    // fixed at <= 50% configured occupancy; cache updates use bounded open
    // addressing and never grow or allocate after the event loop starts.
    std::vector<open_order_cache_entry> open_orders_cache_;
    std::size_t open_orders_cache_size_ = 0;
    std::size_t open_orders_cache_overflow_count_ = 0;
    static constexpr std::size_t kRecentFillsCap = 64;
    std::array<truetest::ui::dashboard_snapshot::fill_row,
               kRecentFillsCap> recent_fills_cache_{};
    std::size_t recent_fills_count_ = 0;
    std::size_t recent_fills_head_ = 0;
    bool dashboard_cache_complete_ = true;

    bool capture_projection(
        truetest::dashboard::DashboardProjection& out,
        std::uint64_t request_epoch,
        bool analytics_quiescent) noexcept;
    bool publish_projection(bool bypass_cadence,
                            bool analytics_quiescent) noexcept;
    std::optional<projection_pin> pin_latest_projection() const noexcept;
    void materialize_dashboard_view(
        const truetest::dashboard::DashboardProjection& projection,
        truetest::ui::dashboard_snapshot& out) const;
    void sample_memory_if_due(truetest::ui::dashboard_snapshot& out) const;

    // Injected data sources (non-owning)
    const portfolio& portfolio_;
    const Analytics& analytics_;
    const AdverseSelectionTracker& adverse_selection_;
    const truetest::exits::ExitManager& exit_manager_;
    const std::atomic<bool>& halt_flag_;
    const std::atomic<std::size_t>& active_order_count_;
    const engine_config& config_;
    const std::atomic<double>& last_mid_price_;
    const std::string& last_mark_symbol_;
    const std::unordered_map<std::string, double>& last_mark_prices_;
    OrderbookRegistry& orderbook_registry_;
    const std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>>& execution_adapters_;
    const std::unordered_set<std::string>& l2_seeded_symbols_;
    std::string provider_name_static_;

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
    DashboardEngineDebugSampler engine_debug_counts_;

    // Debug (optional, may be null)
    DebugSamplers debug_samplers_;

};
