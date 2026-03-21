#pragma once

#include <filesystem>
#include <vector>
#include <utility>
#include <iostream>
#include <chrono>
#include <cstdint>

// Tick side for aggressor identification
enum class data_tick_side : uint8_t
{
	bid = 0,
	ask = 1,
	unknown = 2
};

struct tick_record
{
	std::chrono::system_clock::time_point timestamp;
	std::string symbol;
	double price;
	int64_t quantity;
	data_tick_side side;
};

class data_handler
{

	private:
		size_t current_csv_row_index_ = 0;


	public:
		data_handler() = default;
		void load_from_csv(const std::filesystem::path& path);


		void load_into_queue(std::string date, std::string symbol, double o, double h, double l, double c, int64_t v);


		// OHLCV bar data
		std::vector<std::string>	db_data_date;
		std::vector<std::string>	db_data_symbol;
		std::vector<double>		db_data_open_value;
		std::vector<double>		db_data_high_value;
		std::vector<double>		db_data_low_value;
		std::vector<double>		db_data_close_value;
		std::vector<int64_t>		db_data_volume_value;

		// Tick data (populated by tick data sources)
		std::vector<tick_record>	tick_data;

		bool has_tick_data() const { return !tick_data.empty(); }
		bool has_bar_data() const { return !db_data_symbol.empty(); }
};
