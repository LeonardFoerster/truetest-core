#pragma once
#include "../core/event.h"

class portfolio 
{
public:
    portfolio();
    void on_fill(const fill_event& fill);
    void print_summary() const;
    bool position_open() const { return f_position_open; } // Exposes position state
    std::size_t get_total_trades() const { return total_trades; }

private:

    double f_cash = 0.0; 
    double f_entry_price = 0.0;   
    
    bool f_position_open = false;
    size_t total_trades  = 0;     
};
