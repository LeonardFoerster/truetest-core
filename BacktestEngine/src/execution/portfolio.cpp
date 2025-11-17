#include "portfolio.h"
#include "../strategy/strategy.h"

#include <iostream>


portfolio::portfolio() {};


void portfolio::execute_signal(signal_event signal, double execution_price) 
{
    if (signal == signal_event::buy && !f_position_open)
    {
        f_position_open = true;
        f_entry_price = execution_price;
        std::cout << "buy executed at " << execution_price << std::endl;
    }
    else if (signal == signal_event::sell && f_position_open) 
    {
        total_trades++;

        f_position_open = false; 
        f_entry_price = execution_price;

        std::cout << "sell executed at " << execution_price << std::endl;
    }



}

void portfolio::print_summary() const 
{
    std::cout << "\n--- BACKTEST FINISHED ---" << std::endl;
    std::cout << "Final Portfolio Cash: " << f_cash << std::endl;
    std::cout << "Total Trades Executed: " << total_trades << std::endl;
    std::cout << "-------------------------" << std::endl;
}