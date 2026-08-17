#ifdef HAS_DEBUG

#include "stage_timer.h"

namespace debug {

void StageTimer::record(stage s, std::chrono::high_resolution_clock::time_point start)
{
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - start).count();
    auto idx = static_cast<std::size_t>(s);

    auto& st = stats_[idx];
    st.call_count++;
    st.total_ns += static_cast<uint64_t>(elapsed);
    if (static_cast<uint64_t>(elapsed) > st.max_ns) st.max_ns = static_cast<uint64_t>(elapsed);
    if (static_cast<uint64_t>(elapsed) < st.min_ns) st.min_ns = static_cast<uint64_t>(elapsed);
}

void StageTimer::log() const
{
    // Prefer market_create (bar or tick event creation); fall back to any stage with calls
    uint64_t iterations = stats_[static_cast<size_t>(stage::market_create)].call_count;
    if (iterations == 0)
    {
        for (const auto& st : stats_)
        {
            if (st.call_count > iterations) iterations = st.call_count;
        }
    }
    DBG_PERF("═══ Stage Latency (event loop, %lu iterations) ════", iterations);
    DBG_PERF("  %-18s  %-8s  %-12s  %-10s  %-10s  %-10s",
             "Stage", "Calls", "Total(ms)", "Avg(ns)", "Max(ns)", "Min(ns)");

    for (std::size_t i = 0; i < static_cast<std::size_t>(stage::COUNT); ++i)
    {
        const auto& st = stats_[i];
        if (st.call_count == 0) continue;

        const double total_ms = static_cast<double>(st.total_ns) / 1e6;
        uint64_t avg_ns = st.total_ns / st.call_count;
        uint64_t min_display = (st.min_ns == UINT64_MAX) ? 0 : st.min_ns;

        if (st.max_ns > avg_ns * 10)
            DBG_WARN("  %-18s  %-8lu  %-12.1f  %-10lu  %-10lu  %-10lu  *** JITTER ***",
                     stage_name(static_cast<stage>(i)), st.call_count,
                     total_ms, avg_ns, st.max_ns, min_display);
        else
            DBG_PERF("  %-18s  %-8lu  %-12.1f  %-10lu  %-10lu  %-10lu",
                     stage_name(static_cast<stage>(i)), st.call_count,
                     total_ms, avg_ns, st.max_ns, min_display);
    }
}

void StageTimer::reset()
{
    for (auto& st : stats_)
    {
        st.call_count = 0;
        st.total_ns = 0;
        st.max_ns = 0;
        st.min_ns = UINT64_MAX;
    }
}

}

#endif
