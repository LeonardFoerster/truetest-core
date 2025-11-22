#include "portfolio.h"
#include "../strategy/strategy.h"

#include <iostream>


portfolio::portfolio() {};


void portfolio::execute_signal(const signal_event& signal, double execution_price) 
{
    if (signal.get_signal() == signal_type::buy && !f_position_open)
    {
        f_position_open = true;
        f_entry_price = execution_price;
        // No per-trade console spam; aggregated reporting handled by backtest_core.
    }
    else if (signal.get_signal() == signal_type::sell && f_position_open) 
    {
        total_trades++;

        f_position_open = false; 
        f_entry_price = execution_price;
    }

}

void portfolio::print_summary() const 
{
    std::cout << "Total Trades Executed: " << total_trades << std::endl;
}
