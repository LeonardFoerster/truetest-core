#pragma once
#include "../data/data_handler.h"
#include "../data/db_connection.h"
#include "../strategy/strategy_interface.h"
#include "../execution/portfolio.h"
#include "../orderbook/orderbook.h"

#include <memory>
#include <queue>

class backtest
{
private:
    std::shared_ptr<data_handler> data_handler_;
    std::shared_ptr<database_connection> db_;
    std::shared_ptr<orderbook> orderbook_;
    std::shared_ptr<IStrategy> strategy_;
    portfolio portfolio_;
    uint64_t next_order_id_ = 0;

public:
    backtest(std::shared_ptr<database_connection> db,
             std::shared_ptr<data_handler> dh,
             std::shared_ptr<orderbook> ob,
             std::shared_ptr<IStrategy> strategy);
    void run();
    void print_summary();
};

