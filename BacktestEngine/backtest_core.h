#pragma once
#include "data_handler.h"
#include "strategy.h"
#include "portfolio.h"
#include <string>

class backtest
{
private:
	data_handler data_handler_;
	strategy strategy_;
	portfolio portfolio_;

	std::string ohlc_data_path_;
	std::string bs_data_path_;

public:
	backtest(const std::string& ohlc_path, const std::string& bs_path);
	void run();
	void print_summary();
};