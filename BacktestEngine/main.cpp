#include "backtest_core.h"
#include <iostream>
#include <string>

int main()
{
    std::cout << "--- Backtesting Engine ---" << std::endl;

    std::filesystem::path ohlc_data_source = "C:\\Users\\Leonard\\aktien_szenarien.csv";
    std::filesystem::path bs_data_source   = "C:\\Users\\Leonard\\Desktop\\options_scenarios.csv";

    backtest engine(ohlc_data_source, bs_data_source);

    engine.run();

    engine.print_summary();

    return 0;
}