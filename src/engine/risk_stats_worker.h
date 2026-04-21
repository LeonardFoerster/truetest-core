#pragma once

#include "../threading/worker.h"
#include "../risk/risk_manager.h"
#include "../execution/portfolio.h"
#include "../analytics/analytics.h"

#include <atomic>

class RiskStatsWorker : public Worker
{
public:
    RiskStatsWorker(RiskManager rm,
                    std::atomic<bool>& halt_flag,
                    double initial_cash = 100000.0,
                    std::size_t rolling_window = 252,
                    double risk_free_rate = 0.0)
        : risk_manager_(std::move(rm))
        , analytics_(initial_cash, rolling_window, risk_free_rate)
        , halt_flag_(halt_flag) {}

    const char* worker_name() const override { return "risk_stats"; }

    void on_event(const event_pointer& ev) override
    {
        events_processed_.fetch_add(1, std::memory_order_relaxed);

        analytics_.on_event(ev);

        if (ev->get_type() == event_type::fill)
        {
            auto& fill = static_cast<const fill_event&>(*ev);
            portfolio_.on_fill(fill);

            auto report = analytics_.generate_report();
            auto action = risk_manager_.check_post_fill(fill, portfolio_, report);
            if (action == risk_action::halt)
                halt_flag_.store(true, std::memory_order_release);
        }
        else if (ev->get_type() == event_type::order)
        {
            auto& order = static_cast<const order_event&>(*ev);
            auto snap = analytics_.snapshot();
            auto action = risk_manager_.check_order(order, portfolio_, snap);
            if (action == risk_action::halt)
                halt_flag_.store(true, std::memory_order_release);
        }
    }

    std::size_t events_processed() const
    {
        return events_processed_.load(std::memory_order_relaxed);
    }

    const Analytics& analytics() const { return analytics_; }

private:
    RiskManager risk_manager_;
    portfolio portfolio_;
    Analytics analytics_;
    std::atomic<bool>& halt_flag_;
    std::atomic<std::size_t> events_processed_{0};
};
