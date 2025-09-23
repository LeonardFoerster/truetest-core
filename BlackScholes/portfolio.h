#pragma once

#include <iostream>
#include "black_scholes.h"
#include "perform_calculations.h"
#include "strategy.h"


class portfolio
{

public:

	portfolio(double starting_value);

	void execute_signal(signal_event signal, double execution_price);
	void print_status() const;


private:
	
	double value = 10000.0;
	double profit = 0.0;
	double return_on_invest = 0.0;

};