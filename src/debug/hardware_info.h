#pragma once
#ifdef HAS_DEBUG

#include <string>
#include <vector>
#include <cstdint>

namespace debug {

struct cache_info
{
    std::string level;
    uint64_t size_bytes;
};

struct hardware_info
{
    std::string cpu_model;
    unsigned logical_cores = 0;
    unsigned physical_cores = 0;
    unsigned numa_nodes = 1;
    uint64_t total_ram_bytes = 0;
    uint64_t available_ram_bytes = 0;
    uint64_t page_size = 0;
    std::vector<std::string> ht_siblings;
    std::vector<cache_info> caches;
    std::string isolated_cores;

    static hardware_info detect();
    void log() const;
};

}

#endif
