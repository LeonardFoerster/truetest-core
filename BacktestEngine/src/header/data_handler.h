#pragma once
#include <filesystem>
#include <vector>
#include <map>
#include <optional>
#include <chrono>

#include "header/market_data.h"

class data_handler
{
	private:
		size_t current_data_index_ = 0;

	public:
		data_handler() = default;
		std::pair<std::vector<market_data_bar>, std::vector<double>> load_data_from_files(const std::filesystem::path& ohlc_path, const std::filesystem::path& bs_path);
		size_t get_csv_size();
		size_t get_line_comma_count_csv(std::ifstream&, std::string &line);

		std::vector<market_data_bar> ohlc_data_;
		std::vector<double> bs_data_;

		struct database
		{
			size_t id = 0;
			std::chrono::year_month_day date{ std::chrono::year{2025}, std::chrono::month{11}, std::chrono::day{2} };
			std::string time;
			double open		= 0;
			double high		= 0;
			double low		= 0;
			double close	= 0;
		};
			

		// Vector fürs speicher der id + ohlc werte
		// vlt irgendwie verketter daten struckturen um datum und zeit zuzuordnen, ohne performance zu verlieren
		// Name überarbeiten -> ohlc data etc wird beim csv parsen verwendent. Das ganze vlt auslagern / zusammen packen
		// vlt eigener data handler für csv?

	
};