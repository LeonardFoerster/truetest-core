#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

enum class order_status
{
    pending,
    open,
    partially_filled,
    filled,
    cancelled,
    rejected
};

class OrderTracker
{
public:
    void set_status(uint64_t order_id, order_status status)
    {
        const auto it = statuses_.find(order_id);
        const bool was_active = it != statuses_.end() && is_active_status(it->second);
        const bool now_active = is_active_status(status);
        statuses_[order_id] = status;

        if (!was_active && now_active)
            active_count_.fetch_add(1, std::memory_order_release);
        else if (was_active && !now_active)
            active_count_.fetch_sub(1, std::memory_order_release);
    }

    order_status get_order_status(uint64_t order_id) const
    {
        auto it = statuses_.find(order_id);
        if (it != statuses_.end())
            return it->second;
        return order_status::pending;
    }

    std::vector<uint64_t> get_open_orders() const
    {
        std::vector<uint64_t> result;
        for (const auto& [id, status] : statuses_)
        {
            if (status == order_status::open ||
                status == order_status::pending ||
                status == order_status::partially_filled)
            {
                result.push_back(id);
            }
        }
        return result;
    }

    bool is_active(uint64_t order_id) const
    {
        const auto it = statuses_.find(order_id);
        return it != statuses_.end() && is_active_status(it->second);
    }

    std::size_t active_count() const
    {
        return active_count_.load(std::memory_order_acquire);
    }

    // This is the only OrderTracker state workers may read. Statuses remain
    // engine-thread-owned; exposing the atomic avoids concurrent map access.
    const std::atomic<std::size_t>& active_count_atomic() const
    {
        return active_count_;
    }

    // Phase A (MC object reuse)
    void reset()
    {
        statuses_.clear();
        active_count_.store(0, std::memory_order_release);
    }

private:
    static bool is_active_status(order_status status)
    {
        return status == order_status::pending ||
               status == order_status::open ||
               status == order_status::partially_filled;
    }

    std::unordered_map<uint64_t, order_status> statuses_;
    // Kept off the status-map cache line: written only by the engine and read
    // by workers as the authoritative pre-trade capacity snapshot.
    alignas(64) std::atomic<std::size_t> active_count_{0};
};
