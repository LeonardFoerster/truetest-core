#include "backtest_core.h"
#include "db_connection.h"
#include <iostream>
#include <string>
#include <pqxx/pqxx>
#include <fstream>


int main()
{
    database_connection dh;
    
    std::cout << dh.establish_connection().has_value() << std::endl;
    
    return 0;

    std::cout << "--- Backtesting Engine ---" << std::endl;

    std::filesystem::path ohlc_data_source = "C:\\Users\\Leonard\\aktien_szenarien.csv";
    std::filesystem::path bs_data_source   = "C:\\Users\\Leonard\\Desktop\\options_scenarios.csv";

    backtest engine(ohlc_data_source, bs_data_source);

    engine.run();

    engine.print_summary();

    return 0;
 
}