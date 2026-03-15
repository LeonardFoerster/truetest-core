#include <iostream>
#include <string>
#include <pqxx/pqxx>
#include <cstdlib>

#include "core/backtest_core.h"
#include "data/db_connection.h"
#include "orderbook/orderbook.h"
#include "strategy/mean_reversion_strategy.h"
#include "strategy/sma_strategy.h"
#include "strategy/ma_crossover_strategy.h"
#include "market_maker/market_maker.h"

void using_csv(std::shared_ptr<IStrategy> strategy)
{
    auto dh = std::make_shared<data_handler>();

    std::string csv_path;
    std::cout << "Enter CSV file path: ";
    std::cin >> csv_path;

    dh->load_from_csv(csv_path);

    auto db = std::make_shared<database_connection>();
    auto ob = std::make_shared<orderbook>();

    MarketMaker mm;
    if (!dh->db_data_close_value.empty())
    {
        mm.add_orders(ob, dh->db_data_close_value.front(), 10);
    }

    backtest bt(db, dh, ob, strategy);
    bt.run();
    bt.print_summary();
}

void using_db(std::shared_ptr<IStrategy> strategy)
{
    auto db = std::make_shared<database_connection>();
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();

    db->establish_connection();
    db->test_connection();
    db->load_data(dh);

    MarketMaker mm;
    if (!dh->db_data_close_value.empty())
    {
        mm.add_orders(ob, dh->db_data_close_value.front(), 10);
    }

    backtest bt(db, dh, ob, strategy);
    bt.run();
    bt.print_summary();
}


int main()
{
    std::cout << "Available strategies:" << std::endl;
    std::cout << "1. Mean Reversion (uses SMA)" << std::endl;
    std::cout << "2. SMA-based Strategy" << std::endl;
    std::cout << "3. MA Crossover (buy above MA, sell below MA)" << std::endl;
    std::cout << "Select strategy (1, 2 or 3): ";
    int strategy_choice;
    std::cin >> strategy_choice;

    std::size_t sma_period = 20;
    if (strategy_choice == 1 || strategy_choice == 2 || strategy_choice == 3)
    {
        std::cout << "Enter SMA period: ";
        std::cin >> sma_period;
    }
    else
    {
        std::cout << "Invalid choice, using Mean Reversion with period 20." << std::endl;
        strategy_choice = 1;
    }

    std::shared_ptr<IStrategy> strategy;
    if (strategy_choice == 1)
    {
        strategy = std::make_shared<mean_reversion_strategy>(sma_period);
    } else if (strategy_choice == 2)
    {
        strategy = std::make_shared<sma_strategy>(sma_period);
    } else if (strategy_choice == 3)
    {
        strategy = std::make_shared<ma_crossover_strategy>(sma_period);
    }

    std::cout << "Select data source:" << std::endl;
    std::cout << "1. Database" << std::endl;
    std::cout << "2. CSV" << std::endl;
    std::cout << "Select data source (1 or 2): ";
    int data_choice;
    std::cin >> data_choice;

    if (data_choice == 2)
    {
        using_csv(strategy);
    }
    else
    {
        if (data_choice != 1)
        {
            std::cout << "Invalid choice, using database." << std::endl;
        }
        using_db(strategy);
    }

    return 0;
}
