#include "header/backtest_core.h"
#include "header/data_handler.h"

#include <iostream>
#include <filesystem>
#include  <vector>

backtest::backtest(const std::filesystem::path &ohlc_path, const std::filesystem::path &bs_path)
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
    size_t size = data_handler_.get_csv_size();
    market_data_bar bar;

    auto data_pair = data_handler_.load_data_from_files(ohlc_data_path_, bs_data_path_);
    auto& ohlc = data_pair.first;
    auto& bs = data_pair.second;
    
    for(int i = 0; i <= size; i++)
    {
       signal_event signal = strategy_.check_for_signal (data_handler_.bs_data_[i], data_handler_.ohlc_data_[i].close);

       if (signal != signal_event::hold)
       {
           portfolio_.execute_signal(signal, data_handler_.ohlc_data_[i].close);
       }
       else
       {
           std::cout << "No case" << std::endl;
       }
    }
    

    std::cout << "Backtest abgeschlossen." << std::endl;
}