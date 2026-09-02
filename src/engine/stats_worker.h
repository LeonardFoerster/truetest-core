#pragma once

#include "../threading/worker.h"
#include "../analytics/analytics.h"

#include <atomic>
#include <cstddef>

class StatsWorker : public Worker
{
public:
    explicit StatsWorker(double initial_cash = 100000.0,
                         std::size_t /*legacy_snapshot_interval*/ = 1000,
                         std::size_t rolling_window = 252, double risk_free_rate = 0.0,
                         std::size_t periods_per_year = 525600,
                         std::size_t max_equity_points = 100000)
        : analytics_(initial_cash, rolling_window, risk_free_rate,
                     periods_per_year, max_equity_points) {}

    const char* worker_name() const override { return "stats"; }

    void on_event(const event_pointer& ev) override
    {
        events_processed_.fetch_add(1, std::memory_order_relaxed);
        analytics_.on_event(ev);
    }

    std::size_t events_processed() const
    {
        return events_processed_.load(std::memory_order_relaxed);
    }

    const Analytics& analytics() const { return analytics_; }
    // Mutable only for cold-path fold of engine research counters after stop.
    Analytics& analytics() { return analytics_; }

private:
    Analytics analytics_;
    std::atomic<std::size_t> events_processed_{0};
};
