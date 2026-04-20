#pragma once
#ifdef HAS_DEBUG

#include "debug_log.h"
#include <atomic>
#include <cstdint>

namespace debug {

struct copy_stats
{
    std::atomic<uint64_t> copies{0};
    std::atomic<uint64_t> moves{0};
    std::atomic<uint64_t> copy_assigns{0};
    std::atomic<uint64_t> move_assigns{0};

    void log(const char* type_name) const
    {
        uint64_t c  = copies.load(std::memory_order_relaxed);
        uint64_t m  = moves.load(std::memory_order_relaxed);
        uint64_t ca = copy_assigns.load(std::memory_order_relaxed);
        uint64_t ma = move_assigns.load(std::memory_order_relaxed);

        if (c > 0 || ca > 0)
            DBG_WARN("  %-20s  copies=%lu  moves=%lu  copy==%lu  move==%lu  *** COPIES DETECTED ***",
                     type_name, c, m, ca, ma);
        else
            DBG_COPY("  %-20s  copies=%lu  moves=%lu  copy==%lu  move==%lu",
                     type_name, c, m, ca, ma);
    }

    void reset()
    {
        copies.store(0, std::memory_order_relaxed);
        moves.store(0, std::memory_order_relaxed);
        copy_assigns.store(0, std::memory_order_relaxed);
        move_assigns.store(0, std::memory_order_relaxed);
    }
};

template <typename Derived>
struct CopyTracker
{
    static copy_stats& stats()
    {
        static copy_stats s;
        return s;
    }

    CopyTracker() = default;
    CopyTracker(const CopyTracker&)            { stats().copies.fetch_add(1, std::memory_order_relaxed); }
    CopyTracker(CopyTracker&&) noexcept        { stats().moves.fetch_add(1, std::memory_order_relaxed); }
    CopyTracker& operator=(const CopyTracker&) { stats().copy_assigns.fetch_add(1, std::memory_order_relaxed); return *this; }
    CopyTracker& operator=(CopyTracker&&) noexcept { stats().move_assigns.fetch_add(1, std::memory_order_relaxed); return *this; }
};

} // namespace debug

#else

namespace debug {
template <typename Derived>
struct CopyTracker {};
} // namespace debug

#endif
