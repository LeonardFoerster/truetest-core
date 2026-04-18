#ifdef HAS_DEBUG

#include "memory_info.h"

#include <fstream>
#include <sstream>
#include <string>

namespace debug {

namespace {

uint64_t parse_proc_status_field(const std::string& field)
{
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
    {
        if (line.find(field) == 0)
        {
            std::istringstream ss(line);
            std::string key;
            uint64_t val;
            ss >> key >> val;
            return val * 1024;
        }
    }
    return 0;
}

}

memory_snapshot memory_snapshot::capture()
{
    memory_snapshot snap;
    snap.timestamp = std::chrono::steady_clock::now();
    snap.rss_bytes = parse_proc_status_field("VmRSS:");
    snap.vm_bytes = parse_proc_status_field("VmSize:");
    snap.peak_rss_bytes = parse_proc_status_field("VmHWM:");

    {
        std::ifstream f("/proc/self/statm");
        if (f.is_open())
        {
            uint64_t size, resident, shared, text, lib, data, dt;
            f >> size >> resident >> shared >> text >> lib >> data >> dt;
            snap.heap_bytes = data * 4096;
        }
    }

    return snap;
}

void MemorySampler::log() const
{
    DBG_MEM("%s", "═══ Memory Usage ═══════════════════════════════════");
    DBG_MEM("  %-18s  %-14s  %-14s  %-14s", "", "Start", "End", "Delta");

    auto fmt_bytes = [](uint64_t bytes) -> std::string
    {
        double mib = bytes / (1024.0 * 1024.0);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f MiB", mib);
        return buf;
    };

    auto fmt_delta = [](int64_t delta) -> std::string
    {
        double mib = delta / (1024.0 * 1024.0);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%+.1f MiB", mib);
        return buf;
    };

    DBG_MEM("  %-18s  %-14s  %-14s  %-14s", "RSS",
            fmt_bytes(start_.rss_bytes).c_str(),
            fmt_bytes(end_.rss_bytes).c_str(),
            fmt_delta(static_cast<int64_t>(end_.rss_bytes) - static_cast<int64_t>(start_.rss_bytes)).c_str());

    DBG_MEM("  %-18s  %-14s  %-14s  %-14s", "Virtual",
            fmt_bytes(start_.vm_bytes).c_str(),
            fmt_bytes(end_.vm_bytes).c_str(),
            fmt_delta(static_cast<int64_t>(end_.vm_bytes) - static_cast<int64_t>(start_.vm_bytes)).c_str());

    DBG_MEM("  %-18s  %-14s  %-14s", "Peak RSS", "",
            fmt_bytes(end_.peak_rss_bytes).c_str());

    DBG_MEM("  %-18s  %-14s  %-14s  %-14s", "Heap",
            fmt_bytes(start_.heap_bytes).c_str(),
            fmt_bytes(end_.heap_bytes).c_str(),
            fmt_delta(static_cast<int64_t>(end_.heap_bytes) - static_cast<int64_t>(start_.heap_bytes)).c_str());
}

} // namespace debug

#endif
