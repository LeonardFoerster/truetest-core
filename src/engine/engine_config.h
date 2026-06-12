#pragma once

#include "risk/risk_manager.h"
#include "threading/thread_preset.h"
#include "threading/spin_policy.h"
#include "execution/instrument.h"
#include "execution/live_safety.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

class IFeeModel;
class IFillModel;
class ILatencyModel;
class IImpactModel;
class IProvider;
class IQueuePositionModel;
class IQueueModel;

namespace truetest::ui { class ConsoleDashboard; }

enum class engine_mode { backtest, shadow, live };

// halt_on_drop: a drop from risk/observer/risk_stats (rings that feed
// halt+shadow) sets halt_flag_. Non-safety rings still drop silently.
// main.inc forces halt_on_drop when mode ∈ {shadow, live}.
enum class ring_drop_policy { allow, halt_on_drop };

// Phase 1 hot-path: pre-reserve object-pool blocks at engine startup so
// runtime grow() never hits the heap on the event loop. When
// forbid_runtime_grow is true, exhaustion triggers pool_exhausted → halt.
struct pool_prewarm_settings
{
    std::size_t market_blocks = 1;
    std::size_t tick_blocks = 1;
    std::size_t order_blocks = 2;
    std::size_t fill_blocks = 1;
    std::size_t l2_update_blocks = 2;
    std::size_t l2_snapshot_blocks = 1;
    std::size_t rejection_blocks = 1;
    std::size_t cancel_blocks = 1;
    std::size_t amend_blocks = 1;
    std::size_t funding_blocks = 1;
    // Phase 4: synthetic/MM orderbook depth (~20 orders/bar/tick accumulation).
    // 18 blocks ≈ 73k slots (covers tick-3600 idle @ ~20 replenishes/tick).
    std::size_t orderbook_order_blocks = 18;
    // 0 = auto (sum of all event-pool capacity slots after prewarm).
    std::size_t control_block_slots = 0;
    bool forbid_runtime_grow = true;
};

struct engine_config
{
    engine_mode mode = engine_mode::backtest;

    std::shared_ptr<IFeeModel> fee_model;
    std::shared_ptr<IFillModel> fill_model;
    std::shared_ptr<ILatencyModel> latency_model;

    // Order → venue delay, stacked on top of latency_model (strategy →
    // order-ready). Used by TradeTapeShadowAdapter and HybridExecutor so
    // fills wait for the wire-latency window before releasing.
    std::shared_ptr<ILatencyModel> wire_latency_model;

    // Slippage model applied by LocalBookAdapter to the reference price
    // for market orders before aggression markup. Null → ZeroImpactModel
    // (silent default, current behaviour). Ignored in engine_mode::live
    // (real exchange supplies real impact).
    std::shared_ptr<IImpactModel> impact_model;

    // When set, market orders on L2-seeded symbols use the live book's
    // walked VWAP as their reference price (the actual price you'd pay
    // for that qty) instead of mid + parametric impact_model. Falls
    // back to mid + impact_model when the symbol has no L2 or the
    // requested qty exceeds the book's depth. Ignored in
    // engine_mode::live.
    bool walked_book_impact = false;

    std::size_t ring_buffer_capacity = 65536;

    thread_preset threading = thread_preset::inline_mode;

    bool disable_pinning = false;

    spin_policy worker_spin_policy = spin_policy::adaptive;

    int pin_event_loop = -1;
    int pin_logging    = -1;
    int pin_risk       = -1;
    int pin_stats      = -1;
    int pin_mm         = -1;

    double initial_balance = 10000.0;

    risk_limits risk = {};
    bool risk_unwind = false;

    uint64_t seed = 0;

    std::string event_log_path;
    bool compress_log = true;

    std::string text_log_path;
    bool log_to_stdout = false;

    std::uint64_t log_max_bytes = 0;
    int log_max_files = 5;

    // When false, suppresses the interactive progress bar ("\rProgress..."),
    // final "Trades executed: ..." line, and "Event throughput" line.
    // Useful for Monte Carlo batch runs and scripted/quiet operation.
    // Default is true to preserve existing single-run behavior.
    bool show_progress = true;

    std::shared_ptr<IProvider> provider;

#ifdef HAS_QUESTDB
    bool persist_enabled = false;
    std::string questdb_host = "127.0.0.1";
    std::uint16_t questdb_ilp_port = 9009;
    std::uint16_t questdb_http_port = 9000;
    std::string run_tag;     // empty → auto-generate
    std::string run_notes;   // optional free-form, goes to runs_meta

    // How often to call QuestdbStore::tick() (time-based ILP flush) during long runs.
    // Lower values reduce risk of buffer loss on quiet periods or crashes.
    // Default 150ms is a good balance (cheap comparison + occasional actual flush).
    std::chrono::milliseconds questdb_flush_cadence{150};

    // Phase 2: When true, QuestDB unavailability at start or persistent write failures
    // cause a hard failure (instead of soft-disabling persistence). Also enables
    // local ILP fallback file writing on ILP failures.
    bool questdb_strict = false;

    // If non-empty and strict mode or fallback is triggered, raw ILP lines are
    // appended here instead of being lost.
    std::string questdb_fallback_path;
#endif

    std::string checkpoint_path;
    std::string resume_checkpoint_path;
    std::size_t checkpoint_interval_events = 10000;

    int backfill_bars = 500;
    std::string backfill_interval;
    std::string backfill_host;

    std::size_t rolling_window = 252;

    double risk_free_rate = 0.0;

    std::size_t periods_per_year = 252;

    std::size_t max_equity_points = 100000;

    std::size_t execution_bar_delay = 1;

    std::unordered_map<std::string, instrument_spec> instrument_overrides;

    // Live-mode only. Null → resolved from provider or safe defaults.
    std::shared_ptr<IReconciler> reconciler;
    std::shared_ptr<IKillSwitch> kill_switch;
    double reconcile_tolerance_bps = 10.0;
    std::chrono::milliseconds kill_switch_deadline{5000};

    double market_aggression = 1.1;
    double qty_scale = 1e8;
    unsigned fill_rng_seed = 42;
    double spread_step_factor = 0.0001;

    // Synthetic MarketMaker calibration for bar-mode backtests. The seeded
    // book is the sole source of spread cost for taker fills, so tune
    // these to the target market. Mirrors mm_calibration (kept as scalars
    // so this header stays free of market_maker.h).
    int mm_levels_per_side = 10;
    int mm_base_depth = 100;
    double mm_base_spread_pct = 0.002;
    double mm_vol_spread_mult = 5.0;

    // Deprecated, ignored: passive-side fill pricing is always on. Fills
    // record the resting counterparty's price, one fill_event per walked
    // level; the aggressor's marked-up book price is only a crossing
    // limit. Field removed together with the frozen engine.cpp callsite
    // in the CCB phase.
    bool realistic_fills = false;

    // Deprecated, ignored: the recorded fill price always comes from the
    // resting book, so a parametric bar-spread shift no longer applies.
    // Calibrate mm_base_spread_pct instead. Removed in the CCB phase.
    double bar_spread_bps = 0.0;

    // Shadow-mode queue-position estimate. Null → NoQueueModel default
    // (legacy fill-on-cross). When set, TradeTapeShadowAdapter holds
    // simulated limits until the real tape has consumed the queue
    // ahead of them. Requires a depth subscription. Ignored in
    // engine_mode::live.
    std::shared_ptr<IQueuePositionModel> queue_position_model;

    // Maker queue model for paper and backtest passive limit orders.
    // When set, the engine uses QueueAwareBookAdapter (instead of
    // LocalBookAdapter) for limit orders. This gives realistic queue
    // position + cancel attribution (Front/Uniform/Back). Requires L2
    // data (depth stream). Ignored in engine_mode::live.
    std::shared_ptr<IQueueModel> maker_queue_model;

    bool debug_fills = false;
    int debug_fills_budget = 20;

    unsigned max_consecutive_worker_errors = 5;

    ring_drop_policy drop_policy = ring_drop_policy::allow;

    pool_prewarm_settings pool_prewarm{};

    std::shared_ptr<truetest::ui::ConsoleDashboard> dashboard;

    bool is_threaded() const { return threading != thread_preset::inline_mode; }
};
