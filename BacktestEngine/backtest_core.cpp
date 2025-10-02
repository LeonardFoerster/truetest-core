#include "backtest_core.h"
#include "data_handler.h"
#include "strategy.h"
#include "portfolio.h"

#include <iostream>


backtest::backtest() {};

void run(std::map<int, double> core_bs_data, std::map <int, market_data_bar> core_ohlc_data)
{
	backtest b;
	data_handler d;
	strategy s;

	//-----------------------
	d.load_data(b);
	b.load_data_in_core();
	

	while (true)
	{



	}

}	