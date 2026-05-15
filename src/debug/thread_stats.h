#pragma once
#ifdef HAS_DEBUG

#include "debug_log.h"
#include <chrono>
#include <cstdint>

namespace debug {

struct thread_utilization
{
    uint64_t busy_ns = 0;
    uint64_t idle_ns = 0;
    uint64_t events_processed = 0;
    uint64_t poll_attempts = 0;
    uint64_t poll_hits = 0;

    double busy_pct() const
    {
        uint64_t total = busy_ns + idle_ns;
        return total > 0 ? (busy_ns * 100.0 / total) : 0.0;
    }

    double hit_pct() const
    {
        return poll_attempts > 0 ? (poll_hits * 100.0 / poll_attempts) : 0.0;
    }

    void log(const char* thread_name) const
    {
        DBG_THR("  %-16s  events=%-8lu  busy=%5.1f%%  idle=%5.1f%%  polls=%-8lu  hit=%5.1f%%",
                thread_name, events_processed, busy_pct(), 100.0 - busy_pct(),
                poll_attempts, hit_pct());
    }
};

}

#endif
