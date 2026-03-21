#pragma once

#include "worker.h"
#include "../risk/risk_manager.h"
#include "../execution/portfolio.h"
#include "../analytics/analytics.h"

#include <atomic>

// Consumes events from the risk ring and runs risk checks.
// Owns its own shadow portfolio and analytics to avoid data races with Core 0.
// When a halt condition is detected, sets a shared atomic flag
// that the engine loop checks each iteration.
class RiskWorker : public Worker
{
public:
    RiskWorker(const RiskManager& rm,
               std::atomic<bool>& halt_flag)
        : risk_manager_(rm)
        , halt_flag_(halt_flag) {}

    void on_event(const event_pointer& ev) override
    {
        events_processed_.fetch_add(1, std::memory_order_relaxed);

        // Feed all events to our own analytics for equity tracking
        analytics_.on_event(ev);

        // Update shadow portfolio on fills
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

private:
    const RiskManager& risk_manager_;
    portfolio portfolio_;
    Analytics analytics_;
    std::atomic<bool>& halt_flag_;
    std::atomic<std::size_t> events_processed_{0};
};
