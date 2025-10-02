#include "backtest_core.h"
#include <iostream>
#include <string>

int main()
{
    std::cout << "--- Backtesting Engine ---" << std::endl;

    std::string ohlc_path = "C:\\Users\\Leonard\\aktien_szenarien.csv";
    std::string bs_path = "C:\\Users\\Leonard\\Desktop\\options_scenarios.csv";

    backtest engine(ohlc_path, bs_path);

    engine.run();

    engine.print_summary();

    return 0;
}