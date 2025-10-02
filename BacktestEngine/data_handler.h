#pragma once
#include "backtest_core.h"

#include "market_data.h"
#include <filesystem>
#include <vector>
#include <map>

enum class data_type_content
{
	ohlc_data,
	black_scholes_data
};

class data_handler
{

private:
	
	std::vector <double> bs_line_data_;
	std::map <int, market_data_bar> ohlc_line_data_;

public:
	
	data_handler();
	bool more_data_available = true;

	std::filesystem::path olhc_data_path_ = "C:\\Users\\Leonard\\aktien_szenarien.csv";
	std::filesystem::path bs_data_path_ = "C:\\Users\\Leonard\\Desktop\\options_scenarios.csv";

	
	std::vector<double> load_bs_data(const std::filesystem::path& data_path, backtest &b);
	std::map<int, market_data_bar> load_olhc_data(const std::filesystem::path& data_path, backtest& b);
	
	void load_data(backtest &b);
			
};
