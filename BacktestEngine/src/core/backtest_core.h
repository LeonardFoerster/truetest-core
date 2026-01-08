#pragma once
#include "../data/data_handler.h"
#include "../data/db_connection.h"
#include "../strategy/mean_reversion_strategy.h"
#include "../execution/portfolio.h"

#include <memory>
#include <queue>

class backtest
{
private:
    std::shared_ptr<data_handler> data_handler_;
    std::shared_ptr<database_connection> db_;
    mean_reversion_strategy strategy_;
    portfolio portfolio_;

public:
    backtest(std::shared_ptr<database_connection> db,
             std::shared_ptr<data_handler> dh,
             std::size_t sma_period = 10);
    void run();
    void print_summary();
};

