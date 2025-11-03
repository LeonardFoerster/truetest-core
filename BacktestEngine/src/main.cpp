#include <iostream>
#include <string>
#include <pqxx/pqxx>
#include <fstream>

#include "..\\header\\backtest_core.h"
#include "..\\header\\db_connection.h"


int main()
{
    database_connection db;
    db.establish_connection();
    db.test_connection();
    db.load_data();
    

    return 0;

    std::cout << "--- Backtesting Engine ---" << std::endl;

    std::filesystem::path ohlc_data_source = "C:\\Users\\Leonard\\aktien_szenarien.csv";
    std::filesystem::path bs_data_source   = "C:\\Users\\Leonard\\Desktop\\options_scenarios.csv";

    backtest engine(ohlc_data_source, bs_data_source);

    engine.run();

    engine.print_summary();

    return 0;
 
}