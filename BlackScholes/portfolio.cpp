#include "portfolio.h"
#include <iostream>
#include "strategy.h"

portfolio::portfolio(double starting_cash)
    : m_cash(starting_cash),
    m_entry_price(0.0),
    m_total_trades(0) 
{
}

void portfolio::execute_signal(signal_event signal, double execution_price) 
{
    if (signal == signal_event::buy && !m_position_open)
    {
        m_position_open = true;
        m_entry_price = execution_price;
        std::cout << "buy executed at " << execution_price << std::endl;
    }
    else if (signal == signal_event::sell && m_position_open) 
    {
        double profit = execution_price - m_entry_price;
        m_total_trades++;

        m_position_open = false; 
        m_entry_price = 0.0;

        std::cout << "sell executed at " << execution_price << " | PNL: " << profit << std::endl;
    }
}

void portfolio::print_summary() const 
{
    std::cout << "\n--- BACKTEST FINISHED ---" << std::endl;
    std::cout << "Final Portfolio Cash: " << m_cash << std::endl;
    std::cout << "Total Trades Executed: " << m_total_trades << std::endl;
    std::cout << "-------------------------" << std::endl;
}