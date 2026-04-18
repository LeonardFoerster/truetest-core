#ifdef HAS_DEBUG

#include "hardware_info.h"
#include "debug_log.h"

#include <fstream>
#include <sstream>
#include <thread>
#include <set>
#include <unistd.h>

namespace debug {

namespace {

std::string read_file_line(const std::string& path)
{
    std::ifstream f(path);
    std::string line;
    if (f.is_open())
        std::getline(f, line);
    return line;
}

uint64_t parse_size_string(const std::string& s)
{
    if (s.empty()) return 0;
    uint64_t val = 0;
    std::size_t pos = 0;
    try { val = std::stoull(s, &pos); }
    catch (...) { return 0; }

    if (pos < s.size())
    {
        char suffix = s[pos];
        if (suffix == 'K' || suffix == 'k') val *= 1024;
        else if (suffix == 'M' || suffix == 'm') val *= 1024 * 1024;
        else if (suffix == 'G' || suffix == 'g') val *= 1024 * 1024 * 1024;
    }
    return val;
}

uint64_t parse_meminfo_field(const std::string& field)
{
    std::ifstream f("/proc/meminfo");
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

hardware_info hardware_info::detect()
{
    hardware_info info;

    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (std::getline(f, line))
        {
            if (line.find("model name") != std::string::npos)
            {
                auto pos = line.find(':');
                if (pos != std::string::npos)
                {
                    info.cpu_model = line.substr(pos + 2);
                }
                break;
            }
        }
    }

    info.logical_cores = std::thread::hardware_concurrency();

    {
        std::set<int> core_ids;
        for (unsigned i = 0; i < info.logical_cores; ++i)
        {
            std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) +
                               "/topology/core_id";
            std::string val = read_file_line(path);
            if (!val.empty())
            {
                try { core_ids.insert(std::stoi(val)); }
                catch (...) {}
            }
        }
        info.physical_cores = core_ids.empty() ? info.logical_cores
                                               : static_cast<unsigned>(core_ids.size());
    }

    {
        std::set<std::string> seen;
        for (unsigned i = 0; i < info.logical_cores; ++i)
        {
            std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) +
                               "/topology/thread_siblings_list";
            std::string val = read_file_line(path);
            if (!val.empty() && seen.find(val) == seen.end())
            {
                seen.insert(val);
                info.ht_siblings.push_back("{" + val + "}");
            }
        }
    }

    {
        unsigned count = 0;
        for (unsigned i = 0; i < 64; ++i)
        {
            std::string path = "/sys/devices/system/node/node" + std::to_string(i);
            std::ifstream f(path + "/cpulist");
            if (f.is_open()) ++count;
            else break;
        }
        info.numa_nodes = count > 0 ? count : 1;
    }

    {
        const char* cache_types[] = {"Data", "Instruction", "Unified", "Unified"};
        const char* cache_labels[] = {"L1d", "L1i", "L2", "L3"};
        for (int idx = 0; idx < 4; ++idx)
        {
            std::string path = "/sys/devices/system/cpu/cpu0/cache/index" +
                               std::to_string(idx) + "/size";
            std::string val = read_file_line(path);
            if (!val.empty())
            {
                cache_info ci;
                ci.level = cache_labels[idx];
                ci.size_bytes = parse_size_string(val);
                info.caches.push_back(ci);
            }
        }
        (void)cache_types;
    }

    info.total_ram_bytes = parse_meminfo_field("MemTotal:");
    info.available_ram_bytes = parse_meminfo_field("MemAvailable:");

    info.page_size = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));

    {
        std::string val = read_file_line("/sys/devices/system/cpu/isolated");
        info.isolated_cores = val.empty() ? "none" : val;
    }

    return info;
}

void hardware_info::log() const
{
    DBG_HW("%s", "═══ Hardware Snapshot ════════════════════════════════");
    DBG_HW("  CPU model       : %s", cpu_model.c_str());
    DBG_HW("  Physical cores  : %u", physical_cores);
    DBG_HW("  Logical cores   : %u", logical_cores);

    std::string siblings;
    for (const auto& s : ht_siblings)
        siblings += s + " ";
    if (!siblings.empty())
        DBG_HW("  HT siblings     : %s", siblings.c_str());

    DBG_HW("  NUMA nodes      : %u", numa_nodes);

    std::string cache_str;
    for (std::size_t i = 0; i < caches.size(); ++i)
    {
        if (i > 0) cache_str += " / ";
        if (caches[i].size_bytes >= 1024 * 1024)
            cache_str += std::to_string(caches[i].size_bytes / (1024 * 1024)) + "M";
        else
            cache_str += std::to_string(caches[i].size_bytes / 1024) + "K";
    }
    if (!cache_str.empty())
    {
        std::string labels;
        for (std::size_t i = 0; i < caches.size(); ++i)
        {
            if (i > 0) labels += "/";
            labels += caches[i].level;
        }
        DBG_HW("  %-16s: %s", labels.c_str(), cache_str.c_str());
    }

    double total_gib = total_ram_bytes / (1024.0 * 1024.0 * 1024.0);
    double avail_gib = available_ram_bytes / (1024.0 * 1024.0 * 1024.0);
    DBG_HW("  System RAM      : %.1f GiB (%.1f GiB available)", total_gib, avail_gib);

    DBG_HW("  Page size       : %lu", page_size);
    DBG_HW("  Isolated cores  : %s", isolated_cores.c_str());
}

} // namespace debug

#endif
