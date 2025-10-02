#pragma once
#include <iostream>
#include "data_handler.h"
#include "strategy.h"
#include "portfolio.h"

class backtest
{
private:
	
	strategy s_;
	portfolio p_;

public:
	backtest();
	void run();

	data_handler data_;
	
	void load_data_in_core(backtest &b) 
	{
		data_.load_olhc_data(data_.olhc_data_path_,b);
		data_.load_bs_data(data_.bs_data_path_, b);
	}

	int count_available_bs_data_ = 0; // In lines
	int count_available_olhc_data_ = 0; // In lines

	bool more_bs_data_available_ = true;
	bool more_olhc_data_available_ = true;


};