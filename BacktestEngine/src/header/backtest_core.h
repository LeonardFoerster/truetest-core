#pragma once
#include "data_handler.h"
#include "strategy.h"
#include "portfolio.h"

#include <string>
#include <map>
#include <vector>
#include <filesystem>

class backtest
{
private:
    data_handler data_handler_;
    strategy strategy_;
    portfolio portfolio_;

    std::filesystem::path ohlc_data_path_;
    std::filesystem::path bs_data_path_;

public:
    backtest(const std::filesystem::path &ohlc_path, const std::filesystem::path &bs_path);
    void run();
    void print_summary();
};

