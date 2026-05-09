#pragma once
#ifdef HAS_DEBUG

#include "debug_log.h"
#include <chrono>
#include <cstdint>
#include <climits>

namespace debug {

enum class stage : uint8_t
{
    market_create,
    strategy,
    orderbook,
    fill_processing,
    ring_publish,
    risk_check,
    mm_replenish,
    stop_check,
    pending_drain,
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
    void log() const;
    void reset();

    // Cheap snapshot for the live Debug tab. No mutex — stats_ is
    // updated from the engine main thread; callers reading from the
    // dashboard thread accept "eventually-consistent" values (no torn
    // reads in practice on x86 for aligned uint64).
    stage_stats snapshot(stage s) const
    {
        return stats_[static_cast<size_t>(s)];
    }

    static const char* stage_name(stage s)
    {
        switch (s) {
            case stage::market_create:   return "market_create";
            case stage::strategy:        return "strategy";
            case stage::orderbook:       return "orderbook";
            case stage::fill_processing: return "fill_processing";
            case stage::ring_publish:    return "ring_publish";
            case stage::risk_check:      return "risk_check";
            case stage::mm_replenish:    return "mm_replenish";
            case stage::stop_check:      return "stop_check";
            case stage::pending_drain:   return "pending_drain";
            case stage::COUNT:           return "?";
        }
        return "?";
    }

private:
    stage_stats stats_[static_cast<size_t>(stage::COUNT)] = {};
};

} // namespace debug

#define DEBUG_STAGE(timer, s) debug::StageTimer::scoped _stage_##s(timer, debug::stage::s)

#else

#define DEBUG_STAGE(...) ((void)0)

#endif
