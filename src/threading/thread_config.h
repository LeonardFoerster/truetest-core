#pragma once

#include <cstddef>
#include <thread>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <string>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

enum class core_role
{
    event_loop,
    logging,
    risk,
    market_maker,
    stats
};

struct core_assignment
{
    core_role role;
    int core_id;
};

inline std::size_t detect_physical_cores()
{
#ifdef __linux__
    std::map<std::pair<int, int>, int> physical_cores;
    for (int i = 0; i < 1024; ++i)
    {
        std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/";

        std::ifstream pkg_file(base + "physical_package_id");
        std::ifstream core_file(base + "core_id");

        if (!pkg_file.is_open() || !core_file.is_open())
            break;

        int pkg_id = 0, core_id = 0;
        pkg_file >> pkg_id;
        core_file >> core_id;

        auto key = std::make_pair(pkg_id, core_id);
        if (physical_cores.find(key) == physical_cores.end())
            physical_cores[key] = i;
    }

    if (!physical_cores.empty())
        return physical_cores.size();
#endif

    auto n = std::thread::hardware_concurrency();
    return (n > 0) ? n : 1;
}

inline std::vector<int> get_physical_core_ids()
{
#ifdef __linux__
    std::map<std::pair<int, int>, int> physical_cores;
    for (int i = 0; i < 1024; ++i)
    {
        std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/";

        std::ifstream pkg_file(base + "physical_package_id");
        std::ifstream core_file(base + "core_id");

        if (!pkg_file.is_open() || !core_file.is_open())
            break;

        int pkg_id = 0, core_id = 0;
        pkg_file >> pkg_id;
        core_file >> core_id;

        auto key = std::make_pair(pkg_id, core_id);
        if (physical_cores.find(key) == physical_cores.end())
            physical_cores[key] = i;
    }

    if (!physical_cores.empty())
    {
        std::vector<int> ids;
        ids.reserve(physical_cores.size());
        for (auto& [key, logical_id] : physical_cores)
            ids.push_back(logical_id);
        return ids;
    }
#endif

    auto n = std::thread::hardware_concurrency();
    if (n == 0) n = 1;
    std::vector<int> ids(n);
    for (unsigned i = 0; i < n; ++i)
        ids[i] = static_cast<int>(i);
    return ids;
}

inline std::vector<core_assignment> build_core_map()
{
    auto core_ids = get_physical_core_ids();
    auto n = core_ids.size();

    std::vector<core_assignment> map;

    if (n < 2)
    {
        map.push_back({core_role::event_loop, -1});
        map.push_back({core_role::logging,    -1});
        map.push_back({core_role::risk,       -1});
        map.push_back({core_role::market_maker, -1});
        map.push_back({core_role::stats,      -1});
    }
    else if (n <= 3)
    {
        map.push_back({core_role::event_loop,   core_ids[0]});
        map.push_back({core_role::logging,      core_ids[1]});
        map.push_back({core_role::risk,         core_ids[1]});
        map.push_back({core_role::market_maker, core_ids[n > 2 ? 2 : 1]});
        map.push_back({core_role::stats,        core_ids[1]});
    }
    else if (n <= 5)
    {
        map.push_back({core_role::event_loop,   core_ids[0]});
        map.push_back({core_role::logging,      core_ids[1]});
        map.push_back({core_role::risk,         core_ids[2]});
        map.push_back({core_role::market_maker, core_ids[3]});
        map.push_back({core_role::stats,        core_ids[n > 4 ? 4 : 1]});
    }
    else
    {
        map.push_back({core_role::event_loop,   core_ids[0]});
        map.push_back({core_role::logging,      core_ids[1]});
        map.push_back({core_role::risk,         core_ids[2]});
        map.push_back({core_role::market_maker, core_ids[3]});
        map.push_back({core_role::stats,        core_ids[4]});
    }

    return map;
}

inline bool pin_current_thread(int core_id)
{
    if (core_id < 0)
        return false;

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    return rc == 0;
#else
    (void)core_id;
    return false;
#endif
}

inline bool pin_to_core(std::thread& t, int core_id)
{
    if (core_id < 0)
        return false;

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    int rc = pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
    return rc == 0;
#else
    (void)t;
    (void)core_id;
    return false;
#endif
}
