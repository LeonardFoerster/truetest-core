#pragma once
#include "strategy.h"

class portfolio 
{
public:
    portfolio();
    void execute_signal(signal_event signal, double execution_price);
    void print_summary() const;

private:

    double f_cash; 

    bool f_position_open;   
    double f_entry_price;   
    int total_trades;     
};