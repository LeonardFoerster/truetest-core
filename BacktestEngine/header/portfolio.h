#pragma once
#include "..\\header\\strategy.h"

class portfolio 
{
public:
    portfolio();
    void execute_signal(signal_event signal, double execution_price);
    void print_summary() const;

private:

    double f_cash = 0.0; 
    double f_entry_price = 0.0;   
    
    bool f_position_open = false;
    size_t total_trades  = 0;     
};