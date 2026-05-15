#ifdef HAS_DEBUG

#include "debug_report.h"
#include "debug_log.h"
#include "../core/event.h"
#include "../data/data_handler.h"
#include "../providers/local/csv_parser.h"

namespace debug {

void DebugReport::log_hardware()
{
    auto hw = hardware_info::detect();
    hw.log();
}

void DebugReport::log_copy_stats()
{
    DBG_COPY("%s", "═══ Copy/Move Report ══════════════════════════════");
    DBG_COPY("  %-20s  %-8s  %-8s  %-8s  %-8s", "Type", "copies", "moves", "copy==", "move==");

    CopyTracker<market_event>::stats().log("market_event");
    CopyTracker<order_event>::stats().log("order_event");
    CopyTracker<fill_event>::stats().log("fill_event");
    CopyTracker<tick_event>::stats().log("tick_event");
    CopyTracker<tick_record>::stats().log("tick_record");
    CopyTracker<bar_record>::stats().log("bar_record");
}

void DebugReport::log_stage_latency(const StageTimer& timer)
{
    timer.log();
}

void DebugReport::log_memory(const MemorySampler& sampler)
{
    sampler.log();
}

void DebugReport::log_thread_utilization(
    const std::vector<std::pair<const char*, const thread_utilization*>>& workers)
{
    DBG_THR("%s", "═══ Thread Utilization ══════════════════════════");
    DBG_THR("  %-16s  %-10s  %-8s  %-8s  %-10s  %-8s",
            "Thread", "Events", "Busy%", "Idle%", "Polls", "Hit%");

    for (const auto& [name, util] : workers)
    {
        if (util)
        {
            util->log(name);
            if (util->busy_pct() > 80.0)
                DBG_WARN("  %s busy%% exceeds 80%% — consider load balancing", name);
        }
    }
}

void DebugReport::log_ring_diagnostics(const std::vector<const ring_diagnostics*>& rings)
{
    DBG_RING("%s", "═══ Ring Buffer Diagnostics ═══════════════════════");
    DBG_RING("  %-18s  %-8s  %-10s  %-10s  %-8s  %-10s  %-8s  %-6s",
             "Ring", "cap", "push", "pop", "drops", "HWM", "(%)", "avg");

    for (const auto* ring : rings)
    {
        if (ring)
            ring->log();
    }
}

void DebugReport::log_all(
    const StageTimer& timer,
    const MemorySampler& sampler,
    const std::vector<std::pair<const char*, const thread_utilization*>>& workers,
    const std::vector<const ring_diagnostics*>& rings)
{
    LOG(INFO) << "";
    LOG(INFO) << "═══════════════════════════════════════════════════════";
    LOG(INFO) << "  TrueTest Debug Report";
    LOG(INFO) << "═══════════════════════════════════════════════════════";
    LOG(INFO) << "";

    log_hardware();
    LOG(INFO) << "";
    log_stage_latency(timer);
    LOG(INFO) << "";
    log_copy_stats();
    LOG(INFO) << "";
    log_memory(sampler);
    LOG(INFO) << "";
    log_thread_utilization(workers);
    LOG(INFO) << "";
    log_ring_diagnostics(rings);
    LOG(INFO) << "";
}

}

#endif
