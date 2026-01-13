#include "data_handler.h"
#include "../core/backtest_core.h"
#include "../data/db_connection.h"
#include "../orderbook/orderbook.h"


#include <sstream>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <iterator>
#include <utility>


void data_handler::load_into_queue(std::string s, double o, double h, double l, double c, int64_t v, std::size_t n)
{
        (void)n;
        db_data_symbol.emplace_back(s);
        db_data_open_value.emplace_back(o);
        db_data_high_value.emplace_back(h);
        db_data_low_value.emplace_back(l);
        db_data_close_value.emplace_back(c);
        db_data_volume_value.emplace_back(v);
}

void data_handler::load_from_csv(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file.good())
    {
        throw std::runtime_error("CSV file error: " + path.string());
    }

    std::string line;
    std::getline(file, line); // skip header

    while (std::getline(file, line))
    {
        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string token;
        std::string symbol;
        double open, high, low, close;
        int64_t volume;

        try
        {
            std::getline(ss, symbol, ',');
            std::getline(ss, token, ',');
            open = std::stod(token);
            std::getline(ss, token, ',');
            high = std::stod(token);
            std::getline(ss, token, ',');
            low = std::stod(token);
            std::getline(ss, token, ',');
            close = std::stod(token);
            std::getline(ss, token, ',');
            volume = std::stoll(token);

            load_into_queue(symbol, open, high, low, close, volume, 0); // n not used
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error parsing CSV line: " << line << " | " << e.what() << std::endl;
        }
    }
}
