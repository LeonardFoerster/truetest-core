#pragma once

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
	
	std::map <int, double> bs_line_data_;
	std::map <int, market_data_bar> ohlc_line_data_;

public:
	
	data_handler();
	bool more_data_available = true;
	
	void load_olhc_data(const std::filesystem::path& data_path);
	int load_bs_data(const std::filesystem::path& data_path);
	void load_data(std::filesystem::path data_path_, data_type_content type);
	
	std::map<int, market_data_bar> set_line_data(std::map <int, market_data_bar> new_data)
	{
		ohlc_line_data_ = new_data;
	}

	std::map<int, market_data_bar> get_line_data() const
	{
		return ohlc_line_data_;
	}

	bool set_more_data_available_false()
	{
		return more_data_available = false;
	}

	bool set_more_data_available_true()
	{
		return more_data_available = true;
	}


};
