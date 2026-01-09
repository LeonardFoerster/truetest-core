#include "portfolio.h"
#include "../core/event.h"

#include <iostream>


portfolio::portfolio() : f_cash(100000.0) {}; // Start with some initial cash


void portfolio::on_fill(const fill_event& fill) 
{
    if (fill.get_side() == order_side::buy && !f_position_open)
    {
        f_position_open = true;
        f_entry_price = fill.get_fill_price();
        f_cash -= fill.get_total_cost();
    }
    else if (fill.get_side() == order_side::sell && f_position_open) 
    {
        total_trades++;
        f_position_open = false;
        f_cash += fill.get_total_cost();
    }

}

void portfolio::print_summary() const 
{
    std::cout << "Ending Cash: " << f_cash << std::endl;
    std::cout << "Total Trades Executed: " << total_trades << std::endl;
}
