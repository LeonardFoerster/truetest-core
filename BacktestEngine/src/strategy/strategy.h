#pragma once

enum class signal_event 
{
    buy,
    sell,
    hold 
};


class strategy
{
private:
    double current_market_price = 0.0;
    double calculated_fair_price = 0.0;

public:
    signal_event check_for_signal (double current_market_price, double calculated_fair_price);
    signal_event simple_moving_average();

};  