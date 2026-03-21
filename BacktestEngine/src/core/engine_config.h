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

    risk_limits risk = {};                           // defaults are permissive

    // Deterministic mode: when non-zero, seeds all RNGs for reproducibility.
    uint64_t seed = 0;

    // Event logging: when non-empty, all events are written to this file.
    std::string event_log_path;

    // Text logging
    std::string text_log_path;
    bool log_to_stdout = false;

    // Helper
    bool is_threaded() const { return threading != thread_preset::inline_mode; }
};
