#pragma once
#include "../data/db_connection.h"

#include <filesystem>
#include <vector>
#include <utility>
#include <iostream>

class database_connection;

class data_handler
{

	private:
		size_t current_csv_row_index_ = 0;


	public:
		data_handler() = default;
		void load_from_csv(const std::filesystem::path& path);

		
		void load_into_queue(std::string date, std::string symbol, double o, double h, double l, double c, int64_t v);


		std::vector<std::string>	db_data_date;
		std::vector<std::string>	db_data_symbol;
		std::vector<double>		db_data_open_value;
		std::vector<double>		db_data_high_value;
		std::vector<double>		db_data_low_value;
		std::vector<double>		db_data_close_value;
		std::vector<int64_t>		db_data_volume_value;
		
		
		// deque f�rs speicher der id + ohlc werte
		// vlt irgendwie verketter daten struckturen um datum und zeit zuzuordnen, ohne performance zu verlieren
		// Name �berarbeiten -> ohlc data etc wird beim csv parsen verwendent. Das ganze vlt auslagern / zusammen packen
		// vlt eigener data handler f�r csv?

	
};