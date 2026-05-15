#pragma once
#ifdef HAS_DEBUG

#include "debug_log.h"
#include <chrono>
#include <cstdint>

namespace debug {

struct memory_snapshot
{
    uint64_t rss_bytes = 0;
    uint64_t vm_bytes = 0;
    uint64_t peak_rss_bytes = 0;
    uint64_t heap_bytes = 0;
    uint64_t pool_bytes = 0;
    uint64_t ring_bytes = 0;
    uint64_t data_bytes = 0;
    std::chrono::steady_clock::time_point timestamp;

    static memory_snapshot capture();
};

class MemorySampler
{
public:
    void set_start(const memory_snapshot& snap) { start_ = snap; }
    void set_end(const memory_snapshot& snap)   { end_ = snap; }

    void log() const;

private:
    memory_snapshot start_;
    memory_snapshot end_;
};

}

#endif
