#include "backtest_core.h"
#include "../data/data_handler.h"
#include "../execution/portfolio.h"

#include <iostream>
#include <vector>
#include <queue>
#include <chrono>
#include <iomanip>
#include <algorithm>

backtest::backtest(std::shared_ptr<database_connection> db,
                   std::shared_ptr<data_handler> dh,
                   std::size_t sma_period)
    : data_handler_(std::move(dh)), db_(std::move(db)), strategy_(sma_period) {}

void backtest::print_summary()
{
    portfolio_.print_summary();
}

void backtest::run()
{
    if (!data_handler_ || !db_) throw std::runtime_error("missing dependencies");

    // Load DB data once if empty.
    // Loading reports progress in-place; see database_connection::load_data.
    if (data_handler_->db_data_symbol.empty()) { db_->load_data(data_handler_); }

    std::queue<event_pointer> events;
    const auto base_ts = std::chrono::system_clock::now();
    const auto n = data_handler_->db_data_symbol.size();
    const auto start = std::chrono::high_resolution_clock::now();
    const std::size_t report_interval = std::max<std::size_t>(std::size_t{1}, n / 100); // ~1% steps

    
    std::cout << "\rexecution: 0.000% | Trades executed: 0" << std::flush;

    for (std::size_t i = 0; i < n; ++i)
    {
        market_event mkt
        (
            base_ts + std::chrono::milliseconds(static_cast<long long>(i)),
            data_handler_->db_data_symbol[i],
            data_handler_->db_data_open_value[i],
            data_handler_->db_data_high_value[i],
            data_handler_->db_data_low_value[i],
            data_handler_->db_data_close_value[i],
            data_handler_->db_data_volume_value[i]
        );
        events.push(std::make_shared<market_event>(mkt));

        
        while (!events.empty())
        {
            auto ev = events.front();
            events.pop();

            if (ev->get_type() == event_type::market)
            {
                auto m = std::static_pointer_cast<market_event>(ev);
                auto sig = strategy_.on_market(*m);
                if (sig) { events.push(std::make_shared<signal_event>(*sig)); }
            }
            else if (ev->get_type() == event_type::signal)
            {
                auto s = std::static_pointer_cast<signal_event>(ev);
                portfolio_.execute_signal(*s, data_handler_->db_data_close_value[i]);
                strategy_.set_position_open(portfolio_.position_open()); // keep strategy in sync
            }
        }

        if ((i % report_interval) == 0 || (i + 1) == n)
        {
            const double progress = ((i + 1) * 100.0) / static_cast<double>(n);
            std::cout << "\rProgress: " << std::fixed << std::setprecision(3) << progress
                      << "% | Trades executed: " << portfolio_.get_total_trades()
                      << std::flush;
        }
    }

    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << std::endl;
    std::cout << "Trades executed: " << portfolio_.get_total_trades()
              << " in " << elapsed_ms << " ms" << std::endl;
    std::cout << "Avg Execution time: " << portfolio_.get_total_trades() / elapsed_ms << "ms" << std::endl;
}
