#pragma once

#include <iostream>


class strategy
{

private:

    //double entry_Price = 0.0;
    //double exit_price = 0.0;

    double current_price = 0.0;
    double fair_price = 0.0;

public:
    strategy(double current_price, double fair_price);
    double buy_option(double current_price, double fair_price);
    double sell_option();


    void set_prices(double user_current_price, double fair_price)
    {
        user_current_price = current_price;
        fair_price = fair_price;
    }

    


};   