#pragma once

#include "../threading/worker.h"
#include "../threading/ring_buffer.h"
#include "../market_maker/market_maker.h"
#include "../types/object_pool.h"
#include "../types/order_id.h"

#include <atomic>
#include <chrono>

static constexpr std::size_t MM_RING_SIZE = 65536;
using MMRing = RingBuffer<event_pointer, MM_RING_SIZE>;

class MarketMakerWorker : public Worker
{
public:
    // Default-constructed calibration keeps the frozen engine.cpp callsite
    // source-compatible until the CCB commit passes the configured values.
    MarketMakerWorker(unsigned seed, MMRing& order_ring,
                      const mm_calibration& cal = {})
        : mm_(seed), order_ring_(order_ring)
    {
        mm_.set_calibration(cal);
    }

    const char* worker_name() const override { return "market_maker"; }

    void on_event(const event_pointer& ev) override
    {
        events_processed_.fetch_add(1, std::memory_order_relaxed);

        if (ev->get_type() != event_type::market)
            return;

        auto& mkt = static_cast<const market_event&>(*ev);
        auto orders = mm_.compute_replenish(mkt.get_close());

        for (const auto& mo : orders)
        {
            auto ts = mkt.get_timestamp();
            auto order_ptr = order_pool_.acquire(
                ts, mkt.get_symbol(),
                order_type::limit,
                mo.side, mo.quantity, mo.price,
                time_in_force::gtc);
            order_ptr->set_order_id(OrderIdGenerator::next());
            order_ptr->set_earliest_eligible_ts(ts);
            order_ring_.try_push(order_ptr);
            orders_generated_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::size_t events_processed() const
    {
        return events_processed_.load(std::memory_order_relaxed);
    }

    std::size_t orders_generated() const
    {
        return orders_generated_.load(std::memory_order_relaxed);
    }

private:
    MarketMaker mm_;
    MMRing& order_ring_;
    ObjectPool<order_event> order_pool_;
    std::atomic<std::size_t> events_processed_{0};
    std::atomic<std::size_t> orders_generated_{0};
};
