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
class IQueuePositionModel;

namespace truetest::ui { class ConsoleDashboard; }

enum class engine_mode { backtest, shadow, live };

// halt_on_drop: a drop from risk/observer/risk_stats (rings that feed
// halt+shadow) sets halt_flag_. Non-safety rings still drop silently.
// main.inc forces halt_on_drop when mode ∈ {shadow, live}.
enum class ring_drop_policy { allow, halt_on_drop };

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

#ifdef HAS_QUESTDB
    bool persist_enabled = false;
    std::string questdb_host = "127.0.0.1";
    std::uint16_t questdb_ilp_port = 9009;
    std::uint16_t questdb_http_port = 9000;
    std::string run_tag;     // empty → auto-generate
    std::string run_notes;   // optional free-form, goes to runs_meta
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

    // Opt into passive-side fill pricing. Default off for byte-identical
    // backtest output. When on, market and marketable-limit orders record
    // each fill at the resting counterparty's price (one fill_event per
    // walked level) instead of the aggressor's marked-up book price.
    // Auto-suppresses bar_spread_bps to avoid double-counting the seeded
    // book's spread. Ignored in engine_mode::live.
    bool realistic_fills = false;

    // Calibrated full bid-ask charged to bar-mode market orders. The
    // reference price is shifted by ±(bar_spread_bps/2)/1e4 × mid before
    // impact and aggression. Suppressed for symbols carrying real L2
    // depth, and suppressed under realistic_fills (the seeded MM spread
    // already drives the fill price). Ignored in engine_mode::live.
    double bar_spread_bps = 0.0;

    // Shadow-mode queue-position estimate. Null → NoQueueModel default
    // (legacy fill-on-cross). When set, TradeTapeShadowAdapter holds
    // simulated limits until the real tape has consumed the queue
    // ahead of them. Requires a depth subscription. Ignored in
    // engine_mode::live.
    std::shared_ptr<IQueuePositionModel> queue_position_model;

    bool debug_fills = false;
    int debug_fills_budget = 20;

    unsigned max_consecutive_worker_errors = 5;

    ring_drop_policy drop_policy = ring_drop_policy::allow;

    std::shared_ptr<truetest::ui::ConsoleDashboard> dashboard;

    bool is_threaded() const { return threading != thread_preset::inline_mode; }
};
