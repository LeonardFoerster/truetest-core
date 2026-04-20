#pragma once
#ifdef HAS_DEBUG

#include "hardware_info.h"
#include "stage_timer.h"
#include "memory_info.h"
#include "copy_tracker.h"
#include "thread_stats.h"
#include "ring_stats.h"

#include <vector>

namespace debug {

class DebugReport
{
public:
    void log_hardware();
    void log_copy_stats();
    void log_stage_latency(const StageTimer& timer);
    void log_memory(const MemorySampler& sampler);
    void log_thread_utilization(const std::vector<std::pair<const char*, const thread_utilization*>>& workers);
    void log_ring_diagnostics(const std::vector<const ring_diagnostics*>& rings);

    void log_all(const StageTimer& timer,
                 const MemorySampler& sampler,
                 const std::vector<std::pair<const char*, const thread_utilization*>>& workers,
                 const std::vector<const ring_diagnostics*>& rings);
};

} // namespace debug

#endif
