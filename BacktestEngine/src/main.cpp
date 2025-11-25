#include <iostream>
#include <string>
#include <pqxx/pqxx>
#include <fstream>
#include <cstdlib>

#include "core/backtest_core.h"
#include "data/db_connection.h"


int main()
{
    auto db = std::make_shared<database_connection>();
    auto dh = std::make_shared<data_handler>();
    
    db->establish_connection();
    db->test_connection();
    db->load_data(dh); 

    backtest bt(db, dh, /*sma_period=*/20); 
    bt.run();
    bt.print_summary();

    return 0;

}
