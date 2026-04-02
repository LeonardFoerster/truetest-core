#pragma once

#include "../risk/risk_manager.h"
#include "../threading/thread_preset.h"

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

    // Explicit core pinning overrides. -1 = auto from build_core_map().
    int pin_event_loop = -1;
    int pin_logging    = -1;
    int pin_risk       = -1;
    int pin_stats      = -1;
    int pin_mm         = -1;

    double initial_balance = 10000.0;                 // starting cash

    risk_limits risk = {};                           // defaults are permissive

    // Deterministic mode: when non-zero, seeds all RNGs for reproducibility.
    uint64_t seed = 0;

    // Event logging: when non-empty, all events are written to this file.
    std::string event_log_path;
    bool compress_log = true;            // zstd-compress binary event logs

    // Text logging
    std::string text_log_path;
    bool log_to_stdout = false;

    // Provider: when set, the engine uses this provider's transport and
    // execution adapter instead of manually wired sources.
    // This is scaffolding — full provider-based engine wiring comes later.
    std::shared_ptr<IProvider> provider;

    // SQLite persistence: when non-empty, trades/portfolio/equity are stored
    std::string db_path;

    // Historical backfill: fetch N bars from REST API before streaming starts
    int backfill_bars = 500;          // Number of historical bars to fetch on start
    std::string backfill_interval;    // Kline interval for backfill (default: match stream)
    std::string backfill_host;        // REST host override (empty = auto-detect from provider)

    // WebSocket UI: when enabled, streams events to browser clients
    bool enable_web_ui = false;
    uint16_t ws_port = 8765;

    // Helper
    bool is_threaded() const { return threading != thread_preset::inline_mode; }
};
