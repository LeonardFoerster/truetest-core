#pragma once
#include "../utils/market_data.h"
#include "../data/db_connection.h"

#include <filesystem>
#include <vector>
#include <map>
#include <optional>
#include <chrono>
#include <utility>
#include <iostream>
#include <deque>
#include <memory>
#include <cstdint> 

class database_connection;

class data_handler
{

	private:
		size_t current_csv_row_index_ = 0;
		

	public:
		data_handler() = default;
		std::pair<std::vector<market_data_bar>, std::vector<double>> load_data_from_files(const std::filesystem::path& ohlc_path, const std::filesystem::path& bs_path);
		size_t get_csv_size();
		size_t get_csv_line_comma_count(std::ifstream&, std::string &line);


		std::vector<market_data_bar> ohlc_file_data_vector_;
		std::vector<double> bs_file_data_vector_;

		
		void load_into_queue(std::string, double, double, double, double, int64_t, std::size_t );

		
		std::deque<std::string>	db_data_symbol;
		std::deque<double>		db_data_open_value;
		std::deque<double>		db_data_high_value;
		std::deque<double>		db_data_low_value;
		std::deque<double>		db_data_close_value;
		std::deque<int64_t>		db_data_volume_value;
		
		
		// deque fürs speicher der id + ohlc werte
		// vlt irgendwie verketter daten struckturen um datum und zeit zuzuordnen, ohne performance zu verlieren
		// Name überarbeiten -> ohlc data etc wird beim csv parsen verwendent. Das ganze vlt auslagern / zusammen packen
		// vlt eigener data handler für csv?

	
};