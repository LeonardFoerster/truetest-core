#pragma once
#include "market_data.h"

#include <filesystem>
#include <vector>
#include <map>
#include <optional>

class data_handler
{
	private:
		size_t current_data_index_ = 0;

	public:
		data_handler() = default;
		std::pair<std::vector<market_data_bar>, std::vector<double>> load_data_from_files(const std::filesystem::path& ohlc_path, const std::filesystem::path& bs_path);
		size_t get_csv_size();
		size_t get_line_comma_count(std::ifstream&, std::string &line);

		std::vector<market_data_bar> ohlc_data_;
		std::vector<double> bs_data_;


};