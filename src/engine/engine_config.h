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
class IProvider;

namespace truetest::ui { class ConsoleDashboard; }

enum class engine_mode { backtest, shadow, live };

// How the engine reacts when a worker ring buffer rejects an event push.
// - allow: drop silently, increment the per-ring counter, keep running.
//   Correct for backtest — losing a stats/log event doesn't corrupt state.
// - halt_on_drop: on a drop from a safety-critical ring (risk, observer,
//   risk_stats — the ones that feed the halt flag and shadow portfolio),
//   set halt_flag_ and print one stderr line. Non-safety rings still drop
//   silently even under this policy.
// main.inc forces halt_on_drop when mode ∈ {shadow, live}.
enum class ring_drop_policy { allow, halt_on_drop };

struct engine_config
{
    engine_mode mode = engine_mode::backtest;

    std::shared_ptr<IFeeModel> fee_model;
    std::shared_ptr<IFillModel> fill_model;
    std::shared_ptr<ILatencyModel> latency_model;

    // Extra wire + exchange-ingest latency layered on top of
    // `latency_model`, applied by the execution adapter. Stacks onto
    // `latency_model` because the two model different things:
    //   - `latency_model`       = strategy → order-ready (engine-side)
    //   - `wire_latency_model`  = order → venue (network + ingest)
    //
    // Consumed by:
    //   - TradeTapeShadowAdapter (shadow mode) — gates when real trade
    //     prints can match open orders, so ShadowTracker surfaces
    //     sim/exchange fill divergence caused by network delay.
    //   - HybridExecutor (backtest against a paper-seeded book) — holds
    //     fills in a release buffer until the wire-latency window has
    //     elapsed, so backtests reflect "the exchange ack arrived late"
    //     behavior instead of zero-latency paper.
    std::shared_ptr<ILatencyModel> wire_latency_model;

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

    std::shared_ptr<IProvider> provider;

    std::string db_path;

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

    // Live-mode safety. Only consulted when mode == engine_mode::live.
    // If left null, the engine resolves from the provider; if the provider
    // offers nothing, the engine uses safe no-op defaults.
    std::shared_ptr<IReconciler> reconciler;
    std::shared_ptr<IKillSwitch> kill_switch;
    double reconcile_tolerance_bps = 10.0;
    std::chrono::milliseconds kill_switch_deadline{5000};

    bool enable_web_ui = false;
    uint16_t ws_port = 8765;
    bool ws_compress = true;

    double market_aggression = 1.1;
    double qty_scale = 1e8;
    unsigned fill_rng_seed = 42;
    double spread_step_factor = 0.0001;

    bool debug_fills = false;
    int debug_fills_budget = 20;

    unsigned max_consecutive_worker_errors = 5;

    ring_drop_policy drop_policy = ring_drop_policy::allow;

    // Optional: a live console dashboard that replaces the historical
    // \r-overwrite status line in engine::run_streaming. The engine
    // increments atomic counters in dashboard->stats() on the hot path and
    // emits notable events (connection, backfill, fills, halts, ring drops)
    // via dashboard->push_event. Left null in tests and batch replay where
    // the old behavior is fine.
    std::shared_ptr<truetest::ui::ConsoleDashboard> dashboard;

    bool is_threaded() const { return threading != thread_preset::inline_mode; }
};
