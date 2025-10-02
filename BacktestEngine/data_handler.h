#pragma once
#include "market_data.h"
#include <filesystem>
#include <vector>
#include <map>
#include <optional>

class data_handler
{
private:
	std::map<int, market_data_bar> ohlc_data_;
	std::vector<double> bs_data_;
	size_t current_data_index_ = 0;

public:
	data_handler() = default;
	void load_data_from_files(const std::filesystem::path& ohlc_path, const std::filesystem::path& bs_path);
	std::optional<market_data_bar> get_next_market_data();
};