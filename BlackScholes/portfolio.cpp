#include <iostream>
#include "portfolio.h"


void portfolio::execute_signal(signal_event signal, double execution_price)
{


}


portfolio::portfolio(signal_event signal, double execution_price)
	: signal_event(signal), execution_price(execution_price)
{
}