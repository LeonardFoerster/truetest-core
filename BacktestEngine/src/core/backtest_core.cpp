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
                   std::shared_ptr<orderbook> ob,
                   std::shared_ptr<IStrategy> strategy)
    : data_handler_(std::move(dh)), db_(std::move(db)), orderbook_(std::move(ob)), strategy_(std::move(strategy)) {}

void backtest::print_summary()
{
    portfolio_.print_summary();
}

void backtest::run()
{
    if (!data_handler_ || !db_ || !orderbook_) throw std::runtime_error("missing dependencies");

    
    if (data_handler_->db_data_symbol.empty()) { db_->load_data(data_handler_); }

    const auto base_ts = std::chrono::system_clock::now();
    const auto n = data_handler_->db_data_symbol.size();
    const auto start = std::chrono::high_resolution_clock::now();
    const std::size_t report_interval = std::max<std::size_t>(std::size_t{1}, n / 10);

    std::cout << "\rexecution: 0.000% | Trades executed: 0" << std::flush;

    const std::size_t batch_size = 1000;
    std::vector<market_event> batch_events;
    batch_events.reserve(batch_size);

    std::size_t event_count = 0;

    for (std::size_t i = 0; i < n; ++i)
    {
        
        market_event mkt(
            base_ts + std::chrono::milliseconds(static_cast<long long>(i)),
            data_handler_->db_data_symbol[i],
            data_handler_->db_data_open_value[i],
            data_handler_->db_data_high_value[i],
            data_handler_->db_data_low_value[i],
            data_handler_->db_data_close_value[i],
            data_handler_->db_data_volume_value[i]
        );
        batch_events.push_back(mkt);

        
        if (batch_events.size() == batch_size || i == n - 1)
        {
            std::queue<event_pointer> events;
            for (const auto& mkt : batch_events)
            {
                events.push(std::make_shared<market_event>(mkt));
            }

            while (!events.empty())
            {
                auto ev = events.front();
                events.pop();
                

                switch (ev->get_type())
                {
                    case event_type::market:
                    {
                        auto m = std::static_pointer_cast<market_event>(ev);
                        auto order_opt = strategy_->on_market(*m);
                        if (order_opt)
                        {
                            order_opt->set_order_id(++next_order_id_);
                            events.push(std::make_shared<order_event>(*order_opt));
                        }
                        break;
                    }
                    case event_type::order:
                    {
                        auto o = std::static_pointer_cast<order_event>(ev);

                        ob_order_type book_order_type = ob_order_type::good_till_cancel; 
                        side book_side = (o->get_side() == order_side::buy) ? side::buy : side::sell;
                        
                        price book_price = static_cast<price>(o->get_price() * 100); 
                        quantity book_quantity = static_cast<quantity>(o->get_quantity());

                        auto book_order = std::make_shared<order>(book_order_type, o->get_order_id(), book_side, book_price, book_quantity);
                        
                        trades resulting_trades = orderbook_->add_order(book_order);

                        for (const auto& trade : resulting_trades)
                        {
                            const auto& our_trade_info = (trade.get_bid_trade().orderId_ == o->get_order_id()) ? trade.get_bid_trade() : trade.get_ask_trade();
                            
                            if (our_trade_info.orderId_ == o->get_order_id())
                            {
                                auto fill = std::make_shared<fill_event>(
                                    o->get_timestamp(),
                                    o->get_symbol(),
                                    o->get_order_id(),
                                    o->get_side(),
                                    static_cast<int>(our_trade_info.quantity_),
                                    static_cast<double>(our_trade_info.price_) / 100.0 
                                );
                                events.push(fill);
                            }
                        }
                        break;
                    }
                    case event_type::fill:
                    {
                        auto f = std::static_pointer_cast<fill_event>(ev);
                        portfolio_.on_fill(*f);
                        strategy_->set_position_open(portfolio_.position_open()); 
                        break;
                    }
                    case event_type::signal:
                        break;
                }
                event_count++;
            }

            batch_events.clear();
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
              << " in " << (elapsed_ms > 0 ? elapsed_ms : 1) << " ms" << std::endl;

    double throughput = static_cast<double>(event_count) / (elapsed_ms / 1000.0);
    std::cout << "Event throughput: " << throughput << " events/second" << std::endl;
}
