#pragma once
#ifdef HAS_DEBUG

#include "debug_log.h"
#include <chrono>
#include <cstdint>
#include <climits>

namespace debug {

enum class stage : uint8_t
{
    market_create,      // constructing the market_event
    strategy,           // IStrategy::on_market() / on_tick()
    orderbook,          // orderbook add + match
    fill_processing,    // portfolio::on_fill() + fill event creation
    ring_publish,       // publishing to worker ring buffers
    risk_check,         // inline risk check (non-threaded mode)
    mm_replenish,       // market maker replenishment
    stop_check,         // pending stop order scan
    pending_drain,      // pending latency-delayed order drain
    COUNT
};

struct stage_stats
{
    uint64_t call_count = 0;
    uint64_t total_ns = 0;
    uint64_t max_ns = 0;
    uint64_t min_ns = UINT64_MAX;
};

class StageTimer
{
public:
    struct scoped
    {
        StageTimer& timer;
        stage s;
        std::chrono::high_resolution_clock::time_point start;

        scoped(StageTimer& t, stage stg)
            : timer(t), s(stg), start(std::chrono::high_resolution_clock::now()) {}
        ~scoped() { timer.record(s, start); }
    };

    void record(stage s, std::chrono::high_resolution_clock::time_point start);
    void log() const;  // logs via DBG_PERF()
    void reset();

private:
    stage_stats stats_[static_cast<size_t>(stage::COUNT)] = {};
};

} // namespace debug

// Convenience macro — vanishes when HAS_DEBUG is off
#define DEBUG_STAGE(timer, s) debug::StageTimer::scoped _stage_##s(timer, debug::stage::s)

#else

#define DEBUG_STAGE(...) ((void)0)

#endif
