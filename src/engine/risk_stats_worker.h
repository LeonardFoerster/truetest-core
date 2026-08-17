#pragma once

#include "../threading/worker.h"
#include "../risk/risk_manager.h"
#include "../execution/portfolio.h"
#include "../analytics/analytics.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <string_view>
#include <utility>

class RiskStatsWorker : public Worker
{
public:
    RiskStatsWorker(RiskManager rm,
                    std::atomic<bool>& halt_flag,
                    const std::atomic<std::size_t>& active_order_count,
                    double initial_cash = 100000.0,
                    std::size_t rolling_window = 252,
                    double risk_free_rate = 0.0,
                    std::size_t periods_per_year = 252,
                    std::size_t max_equity_points = 100000,
                    std::function<void(std::string_view)> halt_cb = {},
                    bool enforce_terminal_halt = true)
        : risk_manager_(std::move(rm))
        , analytics_(initial_cash, rolling_window, risk_free_rate,
                     periods_per_year, max_equity_points)
        , halt_flag_(halt_flag)
        , active_order_count_(active_order_count)
        , halt_cb_(std::move(halt_cb))
        , enforce_terminal_halt_(enforce_terminal_halt) {}

    const char* worker_name() const override { return "risk_stats"; }

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
                request_halt();
        }
        else if (ev->get_type() == event_type::order)
        {
            auto& order = static_cast<const order_event&>(*ev);
            auto snap = analytics_.snapshot();
            auto action = risk_manager_.check_order(order, portfolio_, snap,
                order.get_pretrade_open_order_count());
            if (action == risk_action::halt)
                request_halt();
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
    const std::atomic<std::size_t>& active_order_count_;
    std::function<void(std::string_view)> halt_cb_;
    bool enforce_terminal_halt_;
    std::atomic<std::size_t> events_processed_{0};

    void request_halt()
    {
        if (!enforce_terminal_halt_) return;
        if (halt_cb_) halt_cb_("risk/stats worker requested halt");
        else halt_flag_.store(true, std::memory_order_release);
    }
};
