#pragma once

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
        statuses_[order_id] = status;
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
        auto s = get_order_status(order_id);
        return s == order_status::pending ||
               s == order_status::open ||
               s == order_status::partially_filled;
    }

    std::size_t active_count() const
    {
        std::size_t count = 0;
        for (const auto& [id, status] : statuses_)
        {
            if (status == order_status::open ||
                status == order_status::pending ||
                status == order_status::partially_filled)
                ++count;
        }
        return count;
    }

private:
    std::unordered_map<uint64_t, order_status> statuses_;
};
