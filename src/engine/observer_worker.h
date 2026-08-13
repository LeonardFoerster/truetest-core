#pragma once

#include "../threading/worker.h"
#include "../risk/risk_manager.h"
#include "../execution/portfolio.h"
#include "../analytics/analytics.h"

#include <atomic>

class ObserverWorker : public Worker
{
public:
    ObserverWorker(RiskManager rm,
                   std::atomic<bool>& halt_flag,
                   double initial_cash = 100000.0,
                   std::size_t rolling_window = 252,
                   double risk_free_rate = 0.0,
                   std::size_t periods_per_year = 252,
                   std::size_t max_equity_points = 100000)
        : risk_manager_(std::move(rm))
        , analytics_(initial_cash, rolling_window, risk_free_rate,
                     periods_per_year, max_equity_points)
        , halt_flag_(halt_flag) {}

    const char* worker_name() const override { return "observer"; }

    void on_event(const event_pointer& ev) override
    {
        events_processed_.fetch_add(1, std::memory_order_relaxed);

        analytics_.on_event(ev);

        if (ev->get_type() == event_type::fill)
        {
            auto& fill = static_cast<const fill_event&>(*ev);
            // Use rich on_fill (opener + strategy carried on the fill_event after
            // deepdive stamping in engine poll paths). Falls back gracefully for
            // any legacy fills.
            portfolio_.on_fill(fill, fill.get_opener_order_id(), fill.get_strategy_name());

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
    // Mutable only for cold-path fold of engine research counters after stop.
    Analytics& analytics() { return analytics_; }

private:
    RiskManager risk_manager_;
    portfolio portfolio_;
    Analytics analytics_;
    std::atomic<bool>& halt_flag_;
    std::atomic<std::size_t> events_processed_{0};
};
