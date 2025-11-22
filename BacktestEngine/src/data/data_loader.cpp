#include "data_handler.h"
#include "../core/backtest_core.h"
#include "../data/db_connection.h"
#include "../orderbook/orderbook.h"


#include <sstream>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <iterator>
#include <utility>


void data_handler::load_into_queue(std::string s, double o, double h, double l, double c, int64_t v, std::size_t n)
{

	db_data_symbol.emplace_back(s);
	db_data_open_value.emplace_back(o);
	db_data_high_value.emplace_back(h);
	db_data_low_value.emplace_back(l);
	db_data_close_value.emplace_back(c);
	db_data_volume_value.emplace_back(v);
}







