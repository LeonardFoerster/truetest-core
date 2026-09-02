#include "dashboard_snapshot_builder.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

void DashboardSnapshotBuilder::sample_memory_if_due(
    truetest::ui::dashboard_snapshot& out) const
{
    std::lock_guard<std::mutex> lock(memory_cache_mu_);
    const auto now = std::chrono::steady_clock::now();
    const bool stale = !memory_cache_initialised_ ||
        now - memory_cache_last_ >= std::chrono::seconds(1);

    if (stale)
    {
        auto& memory = memory_cache_;
        memory = truetest::ui::dashboard_snapshot::memory_view{};

        std::ifstream status_file("/proc/self/status");
        if (status_file.is_open())
        {
            std::string line;
            int hits = 0;
            while (hits < 3 && std::getline(status_file, line))
            {
                const auto extract_kib = [&](const char* key) -> std::uint64_t {
                    const std::size_t key_length = std::strlen(key);
                    if (line.compare(0, key_length, key) != 0) return 0;
                    std::istringstream stream(line);
                    std::string parsed_key;
                    std::uint64_t value = 0;
                    stream >> parsed_key >> value;
                    return value * 1024;
                };
                if (const auto value = extract_kib("VmRSS:"))
                {
                    memory.rss_bytes = value;
                    ++hits;
                    continue;
                }
                if (const auto value = extract_kib("VmSize:"))
                {
                    memory.vm_bytes = value;
                    ++hits;
                    continue;
                }
                if (const auto value = extract_kib("VmHWM:"))
                {
                    memory.peak_rss_bytes = value;
                    ++hits;
                }
            }
        }

        {
            std::ifstream statm_file("/proc/self/statm");
            if (statm_file.is_open())
            {
                std::uint64_t size = 0;
                std::uint64_t resident = 0;
                std::uint64_t shared = 0;
                std::uint64_t text = 0;
                std::uint64_t library = 0;
                std::uint64_t data = 0;
                std::uint64_t dirty = 0;
                if (statm_file >> size >> resident >> shared >> text >>
                    library >> data >> dirty)
                {
                    memory.heap_bytes = data * 4096;
                }
            }
        }
        memory.available = memory.rss_bytes > 0;

        {
            std::ifstream maps_file("/proc/self/maps");
            std::uint64_t heap_bytes = 0;
            std::uint64_t stack_bytes = 0;
            std::uint64_t shared_object_bytes = 0;
            std::uint64_t anonymous_bytes = 0;
            std::uint64_t file_bytes = 0;
            if (maps_file.is_open())
            {
                std::string line;
                while (std::getline(maps_file, line))
                {
                    const auto dash = line.find('-');
                    if (dash == std::string::npos) continue;
                    const auto space = line.find(' ', dash + 1);
                    if (space == std::string::npos) continue;
                    std::uint64_t begin = 0;
                    std::uint64_t end = 0;
                    try
                    {
                        begin = std::stoull(line.substr(0, dash), nullptr, 16);
                        end = std::stoull(
                            line.substr(dash + 1, space - dash - 1),
                            nullptr, 16);
                    }
                    catch (...)
                    {
                        continue;
                    }
                    if (end <= begin) continue;
                    const auto bytes = end - begin;

                    const auto path_pos = line.find_last_of(' ');
                    const std::string path = path_pos != std::string::npos
                        ? line.substr(path_pos + 1) : std::string{};
                    if (path == "[heap]") heap_bytes += bytes;
                    else if (path.rfind("[stack", 0) == 0) stack_bytes += bytes;
                    else if (path.find(".so") != std::string::npos)
                        shared_object_bytes += bytes;
                    else if (path.empty() || path[0] == '[')
                        anonymous_bytes += bytes;
                    else file_bytes += bytes;
                }
            }
            if (heap_bytes > 0)
                memory.other_breakdown.push_back({"heap", heap_bytes});
            if (stack_bytes > 0)
                memory.other_breakdown.push_back({"stacks", stack_bytes});
            if (shared_object_bytes > 0)
                memory.other_breakdown.push_back(
                    {".so libs", shared_object_bytes});
            if (file_bytes > 0)
                memory.other_breakdown.push_back({"file-mmap", file_bytes});
            if (anonymous_bytes > 0)
                memory.other_breakdown.push_back(
                    {"anon-mmap", anonymous_bytes});
        }

        constexpr std::size_t pool_block_size = 4096;
        const auto add_pool = [&](const char* name, std::size_t blocks,
                                  std::size_t slot_size,
                                  std::size_t in_use,
                                  std::size_t grow_count) {
            using snapshot_t = truetest::ui::dashboard_snapshot;
            snapshot_t::mem_pool_row row;
            row.name = name;
            row.blocks = blocks;
            row.slot_size = slot_size;
            row.bytes = static_cast<std::uint64_t>(blocks) *
                        pool_block_size * slot_size;
            row.in_use = in_use;
            row.capacity_slots = blocks * pool_block_size;
            row.grow_count = grow_count;
            memory.pool_bytes_total += row.bytes;
            memory.pools.push_back(row);
        };
        add_pool("market_pool", market_pool_.block_count(),
                 sizeof(market_event), market_pool_.in_use(),
                 market_pool_.grow_count());
        add_pool("order_pool", order_pool_.block_count(), sizeof(order_event),
                 order_pool_.in_use(), order_pool_.grow_count());
        add_pool("fill_pool", fill_pool_.block_count(), sizeof(fill_event),
                 fill_pool_.in_use(), fill_pool_.grow_count());
        add_pool("tick_pool", tick_pool_.block_count(), sizeof(tick_event),
                 tick_pool_.in_use(), tick_pool_.grow_count());
        add_pool("l2_update_pool", l2_update_pool_.block_count(),
                 sizeof(l2_update_event), l2_update_pool_.in_use(),
                 l2_update_pool_.grow_count());
        add_pool("l2_snapshot_pool", l2_snapshot_pool_.block_count(),
                 sizeof(l2_snapshot_event), l2_snapshot_pool_.in_use(),
                 l2_snapshot_pool_.grow_count());
        add_pool("rejection_pool", rejection_pool_.block_count(),
                 sizeof(rejection_event), rejection_pool_.in_use(),
                 rejection_pool_.grow_count());
        add_pool("cancel_pool", cancel_pool_.block_count(),
                 sizeof(cancel_event), cancel_pool_.in_use(),
                 cancel_pool_.grow_count());
        add_pool("amend_pool", amend_pool_.block_count(),
                 sizeof(amend_event), amend_pool_.in_use(),
                 amend_pool_.grow_count());
        add_pool("funding_pool", funding_pool_.block_count(),
                 sizeof(funding_event), funding_pool_.in_use(),
                 funding_pool_.grow_count());
        add_pool("control_block_pool", control_block_pool_.block_count(),
                 ControlBlockPool::slot_size(), control_block_pool_.in_use(),
                 control_block_pool_.grow_count());

        const auto add_ring = [&](const char* name, const auto& ring) {
            using snapshot_t = truetest::ui::dashboard_snapshot;
            snapshot_t::mem_ring_row row;
            row.name = name;
            if (ring)
            {
                row.capacity = ring->capacity();
                row.element_bytes = sizeof(event_pointer);
                row.bytes = static_cast<std::uint64_t>(row.capacity) *
                            row.element_bytes;
                memory.ring_bytes_total += row.bytes;
            }
            memory.rings.push_back(row);
        };
        add_ring("logging", logging_ring_);
        add_ring("risk", risk_ring_);
        add_ring("stats", stats_ring_);
        add_ring("observer", observer_ring_);
        add_ring("risk_stats", risk_stats_ring_);
        add_ring("mm_event", mm_ring_);

        memory_cache_last_ = now;
        memory_cache_initialised_ = true;
    }
    out.memory = memory_cache_;

    for (auto& pool : out.memory.pools)
    {
        const std::string_view name = pool.name ? pool.name : "";
        if (name == "market_pool") pool.in_use = market_pool_.in_use();
        else if (name == "order_pool") pool.in_use = order_pool_.in_use();
        else if (name == "fill_pool") pool.in_use = fill_pool_.in_use();
        else if (name == "tick_pool") pool.in_use = tick_pool_.in_use();
    }
}
