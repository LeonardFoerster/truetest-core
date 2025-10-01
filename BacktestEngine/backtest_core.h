#pragma once
#include <iostream>
#include "data_handler.h"
#include "strategy.h"
#include "portfolio.h"

class backtest
{
private:
	
	data_handler data_;
	strategy s_;
	portfolio p_;

public:
	backtest();
	void run();
	void load_data_into_core();

	bool data_available_ = true;



};