#pragma once
#include <string>


struct market_data_bar
{
	double open = 0.0;
	double high = 0.0;
	double low = 0.0;
	double close = 0.0;

	bool is_valid = true;
};

