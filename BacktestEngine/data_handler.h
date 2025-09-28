#pragma once

#include "market_data.h"
#include <filesystem>
#include <vector>
#include <map>

class data_handler
{

private:

	std::filesystem::path data_path_ = "C:\\Users\\Leonard\\aktien_szenarien.csv";
	std::map <int, market_data_bar> line_data_;

public:
	data_handler(const std::filesystem::path& file_path);
	bool more_data_available = true;
	
	void load_data(const std::filesystem::path& data_path);
	void get_next_bar() const;
	
	std::map<int, market_data_bar> set_line_data(std::map <int, market_data_bar> new_data)
	{
		line_data_ = new_data;
	}

	std::map<int, market_data_bar> get_line_data() const
	{
		return line_data_;
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
