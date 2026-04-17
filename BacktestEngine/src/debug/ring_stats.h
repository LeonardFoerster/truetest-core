#pragma once
#ifdef HAS_DEBUG

#include "debug_log.h"
#include <cstdint>
#include <algorithm>

namespace debug {

struct ring_diagnostics
{
    const char* name = "";
    uint64_t capacity = 0;
    uint64_t total_pushed = 0;
    uint64_t total_popped = 0;
    uint64_t drop_count = 0;
    uint64_t high_water_mark = 0;
    uint64_t samples = 0;
    uint64_t occupancy_sum = 0;

    void on_push(std::size_t current_occupancy)
    {
        total_pushed++;
        samples++;
        occupancy_sum += current_occupancy;
        high_water_mark = std::max(high_water_mark, static_cast<uint64_t>(current_occupancy));
    }

    void on_drop() { drop_count++; }
    void on_pop()  { total_popped++; }

    double avg_occupancy() const
    {
        return samples > 0 ? static_cast<double>(occupancy_sum) / samples : 0.0;
    }

    void log() const
    {
        if (capacity == 0)
        {
            DBG_RING("  %-18s  (inactive)", name);
            return;
        }

        double hwm_pct = capacity > 0 ? (high_water_mark * 100.0 / capacity) : 0.0;

        if (hwm_pct > 50.0 || drop_count > 0)
            DBG_WARN("  %-18s  cap=%-6lu  push=%-8lu  pop=%-8lu  drops=%-4lu  HWM=%-6lu (%.1f%%)  avg=%.1f",
                     name, capacity, total_pushed, total_popped, drop_count,
                     high_water_mark, hwm_pct, avg_occupancy());
        else
            DBG_RING("  %-18s  cap=%-6lu  push=%-8lu  pop=%-8lu  drops=%-4lu  HWM=%-6lu (%.1f%%)  avg=%.1f",
                     name, capacity, total_pushed, total_popped, drop_count,
                     high_water_mark, hwm_pct, avg_occupancy());
    }
};

} // namespace debug

#endif
