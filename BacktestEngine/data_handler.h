#pragma once
#include  <iostream>
#include <fstream>

struct scenario_data
{
	double current_price = 0.0;
	double strike_price = 0.0;
	double interest_rate = 0.0;
	double volatility = 0.0;
	double duration = 0.0;
	double dividend = 0.0;

	bool valid = false;

};

class data_handler
{
	
};