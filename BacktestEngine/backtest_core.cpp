#include "backtest_core.h"
#include <iostream>

backtest::backtest(const std::string& ohlc_path, const std::string& bs_path)
    : ohlc_data_path_(ohlc_path), bs_data_path_(bs_path)
{
}

void backtest::print_summary()
{
    portfolio_.print_summary();
}

void backtest::run()
{
    data_handler_.load_data_from_files(ohlc_data_path_, bs_data_path_);

    while (auto market_data = data_handler_.get_next_market_data())
    {
        double calculated_fair_price = 150.0;

        signal_event signal = strategy_.check_for_signal(market_data->close, calculated_fair_price);

        if (signal != signal_event::hold)
        {
            portfolio_.execute_signal(signal, market_data->close);
        }
    }

    std::cout << "Backtest abgeschlossen." << std::endl;
}