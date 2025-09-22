#pragma once

#include <iostream>


class execute 
{

private:

	double current_price;
	bool trade_status = false;


public:

	execute(double current_price);


	void open_trade()
	{
		trade_status = true;
	}

	void close_trade()
	{
		trade_status = false;
	}

};