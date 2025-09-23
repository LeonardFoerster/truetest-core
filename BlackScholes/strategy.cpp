
#include <iostream>
#include "strategy.h"
#include "black_scholes.h"

signal_event strategy::check_for_signal(double current_market_price, double calculated_fair_price)
{

	if (calculated_fair_price > current_market_price)
	{
		return signal_event::buy;
	};
	
	if(current_market_price > calculated_fair_price)
	{
		return signal_event::sell;
	}

	if (current_market_price == calculated_fair_price)
	{
		return signal_event::hold;
	}

}