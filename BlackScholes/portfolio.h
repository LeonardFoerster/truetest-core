#pragma once
#include "strategy.h"

class portfolio 
{
public:
    portfolio(double starting_cash);
    void execute_signal(signal_event signal, double execution_price);
    void print_summary() const;

private:
    double m_cash; 

    bool m_position_open;   
    double m_entry_price;   
    int m_total_trades;     
};