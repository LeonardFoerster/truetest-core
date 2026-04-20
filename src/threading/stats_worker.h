#pragma once

#include "worker.h"
#include "../analytics/analytics.h"

#include <atomic>

class StatsWorker : public Worker
{
public:
    explicit StatsWorker(double initial_cash = 100000.0, std::size_t snapshot_interval = 1000,
                         std::size_t rolling_window = 252, double risk_free_rate = 0.0)
        : analytics_(initial_cash, rolling_window, risk_free_rate), snapshot_interval_(snapshot_interval) {}

    const char* worker_name() const override { return "stats"; }

    void on_event(const event_pointer& ev) override
    {
        events_processed_.fetch_add(1, std::memory_order_relaxed);
        analytics_.on_event(ev);

        if (snapshot_interval_ > 0 &&
            events_processed_.load(std::memory_order_relaxed) % snapshot_interval_ == 0)
        {
            last_snapshot_ = analytics_.snapshot();
        }
    }

    std::size_t events_processed() const
    {
        return events_processed_.load(std::memory_order_relaxed);
    }

    AnalyticsReport last_snapshot() const { return last_snapshot_; }

    const Analytics& analytics() const { return analytics_; }

private:
    Analytics analytics_;
    std::size_t snapshot_interval_;
    std::atomic<std::size_t> events_processed_{0};
    AnalyticsReport last_snapshot_;
};
