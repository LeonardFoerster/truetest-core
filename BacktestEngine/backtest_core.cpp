#include "backtest_core.h"
#include "data_handler.h"
#include "strategy.h"
#include "portfolio.h"


#include <iostream>


backtest::backtest() {};


void load_data_into_core()
{
	data_handler core_dh;

	core_dh.load_olhc_data(core_dh.ohlc_data_path_);
	core_dh.load_bs_data(core_dh.bs_data_path_);

}



void run(std::map<int, double> core_bs_data, std::map <int, market_data_bar> core_ohlc_data)
{
	backtest b;
	portfolio p;
	load_data_into_core();
	strategy s;





}