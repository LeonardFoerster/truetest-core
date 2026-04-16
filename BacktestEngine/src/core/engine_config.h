#pragma once

#include "../risk/risk_manager.h"
#include "../threading/thread_preset.h"
#include "../threading/spin_policy.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class IFeeModel;
class IFillModel;
class ILatencyModel;
class IProvider;

enum class engine_mode { backtest, shadow, live };

struct engine_config
{
    engine_mode mode = engine_mode::backtest;

    std::shared_ptr<IFeeModel> fee_model;           // nullptr = zero fees
    std::shared_ptr<IFillModel> fill_model;          // nullptr = perfect fills
    std::shared_ptr<ILatencyModel> latency_model;    // nullptr = zero latency

    std::size_t ring_buffer_capacity = 65536;        // power of 2

    // Threading preset: auto-detected from hardware or set explicitly.
    // inline_mode = no worker threads (single-threaded).
    thread_preset threading = thread_preset::inline_mode;

    // Override: skip all CPU affinity/pinning calls.
    bool disable_pinning = false;

    // Worker thread spin policy: spin (busy-wait), yield (always yield), adaptive (exponential backoff)
    spin_policy worker_spin_policy = spin_policy::adaptive;

    // Explicit core pinning overrides. -1 = auto from build_core_map().
    int pin_event_loop = -1;
    int pin_logging    = -1;
    int pin_risk       = -1;
    int pin_stats      = -1;
    int pin_mm         = -1;

    double initial_balance = 10000.0;                 // starting cash

    risk_limits risk = {};                           // defaults are permissive
    bool risk_unwind = false;                        // on halt, unwind positions instead of stopping

    // Deterministic mode: when non-zero, seeds all RNGs for reproducibility.
    uint64_t seed = 0;

    // Event logging: when non-empty, all events are written to this file.
    std::string event_log_path;
    bool compress_log = true;            // zstd-compress binary event logs

    // Text logging
    std::string text_log_path;
    bool log_to_stdout = false;

    // L3: log rotation. 0 = disabled. When enabled, both the binary event log
    // and the text log are rotated when they exceed log_max_bytes, keeping
    // log_max_files rotated copies.
    std::uint64_t log_max_bytes = 0;
    int log_max_files = 5;

    // Provider: when set, the engine uses this provider's transport and
    // execution adapter instead of manually wired sources.
    // This is scaffolding — full provider-based engine wiring comes later.
    std::shared_ptr<IProvider> provider;

    // SQLite persistence: when non-empty, trades/portfolio/equity are stored
    std::string db_path;

    // K3: portfolio checkpointing for resume-after-crash. When checkpoint_path
    // is non-empty, the engine writes a binary snapshot of portfolio state
    // every `checkpoint_interval_events` events. When resume_checkpoint_path
    // is non-empty at engine construction, the portfolio is pre-populated from
    // the referenced checkpoint file before the run starts.
    std::string checkpoint_path;
    std::string resume_checkpoint_path;
    std::size_t checkpoint_interval_events = 10000;

    // Historical backfill: fetch N bars from REST API before streaming starts
    int backfill_bars = 500;          // Number of historical bars to fetch on start
    std::string backfill_interval;    // Kline interval for backfill (default: match stream)
    std::string backfill_host;        // REST host override (empty = auto-detect from provider)

    // Rolling analytics window size (number of bars for rolling Sharpe/drawdown)
    std::size_t rolling_window = 252;

    // Risk-free rate (annualized, e.g., 0.05 = 5%)
    double risk_free_rate = 0.0;

    // WebSocket UI: when enabled, streams events to browser clients
    bool enable_web_ui = false;
    uint16_t ws_port = 8765;
    bool ws_compress = true;            // per-message deflate (negotiated in handshake)

    // Execution constants (defaults match prior hardcoded values)
    double market_aggression = 1.1;     // market order price multiplier (buy: price*aggr, sell: price*(2-aggr))
    double qty_scale = 1e8;             // fractional qty → integer scale factor
    unsigned fill_rng_seed = 42;        // RNG seed for fill model
    double spread_step_factor = 0.0001; // spread step = mid * factor

    // N1: worker error tolerance. After this many consecutive on_event()
    // exceptions a worker sets the halt flag. 0 = halt on first error.
    unsigned max_consecutive_worker_errors = 5;

    // Helper
    bool is_threaded() const { return threading != thread_preset::inline_mode; }
};
