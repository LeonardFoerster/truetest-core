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

    std::shared_ptr<IFeeModel> fee_model;
    std::shared_ptr<IFillModel> fill_model;
    std::shared_ptr<ILatencyModel> latency_model;

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

    bool is_threaded() const { return threading != thread_preset::inline_mode; }
};
