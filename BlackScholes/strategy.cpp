
#include <iostream>
#include "strategy.h"
#include "black_scholes.h"

strategy::strategy(double current_price, double fair_price)
	: current_price(current_price), fair_price(fair_price)
{
}


double strategy::buy_option(double current_price, double fair_price)
{

	if (fair_price > current_price)
	{

	}

	return 0;
}

double strategy::sell_option()
{
	
	
	return 0;
}