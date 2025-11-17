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

		
		void move_db_data_into_vector(double open, double high, double low, double close);

		// Change the std::vector to std::deque
		std::vector<double> db_data_open_value;
		std::vector<double> db_data_high_value;
		std::vector<double> db_data_low_value;
		std::vector<double> db_data_close_value;

		//std::deque<double> db_data_open_value;
		//std::deque<double> db_data_high_value;
		//std::deque<double> db_data_low_value;
		//std::deque<double> db_data_close_value;
		
				
		// maybe unnötig
		struct id_assignment
		{
			uint32_t id = 0;
			std::chrono::year_month_day date{ std::chrono::year{2025}, std::chrono::month{11}, std::chrono::day{2} };
			std::string time;
			
		};
		
		// not in use (yet)
		struct db_input_data
		{
			std::vector<double> open;
			std::vector<double> high;
			std::vector<double> low;
			std::vector<double> close;
		};

		std::map<uint32_t, std::pair <std::chrono::year_month_day,std::string>> id_date_storage;
		std::vector<db_input_data> ohlc_database_data;



		// deque fürs speicher der id + ohlc werte
		// vlt irgendwie verketter daten struckturen um datum und zeit zuzuordnen, ohne performance zu verlieren
		// Name überarbeiten -> ohlc data etc wird beim csv parsen verwendent. Das ganze vlt auslagern / zusammen packen
		// vlt eigener data handler für csv?

	
};