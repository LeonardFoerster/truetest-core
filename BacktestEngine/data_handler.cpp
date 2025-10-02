#include "data_handler.h"
#include "black_scholes.h"

#include <sstream>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <iterator>

void data_handler::load_data_from_files(const std::filesystem::path& ohlc_path, const std::filesystem::path& bs_path)
{
    std::ifstream ohlc_file(ohlc_path);
    if (!ohlc_file.good())
    {
        throw std::runtime_error("OHLC Input File error");
    }

    std::string line;
    std::getline(ohlc_file, line);
    int line_count = 0;

    while (std::getline(ohlc_file, line))
    {
        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string token;
        market_data_bar bar;

        try
        {
            std::getline(ss, token, ',');
            bar.open = std::stod(token);
            std::getline(ss, token, ',');
            bar.high = std::stod(token);
            std::getline(ss, token, ',');
            bar.low = std::stod(token);
            std::getline(ss, token, ',');
            bar.close = std::stod(token);
            ohlc_data_[line_count++] = bar;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error parsing OHLC data on line: " << line << " | " << e.what() << std::endl;
        }
    }

    std::ifstream bs_file(bs_path);
    if (!bs_file.good())
    {
        throw std::runtime_error("Black-Scholes Input File error");
    }

    std::getline(bs_file, line);

    while (std::getline(bs_file, line))
    {
        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string value_str;
        std::vector<double> values;

        while (std::getline(ss, value_str, ','))
        {
            try
            {
                values.push_back(std::stod(value_str));
            }
            catch (const std::exception& e)
            {
                std::cerr << "Error parsing BS data on line: " << line << " | " << e.what() << std::endl;
                values.clear();
                break;
            }
        }

        if (values.size() == 6)
        {
            black_scholes option_calculator(values[0], values[1], values[2], values[3], values[4], values[5]);
            bs_data_.push_back(option_calculator.get_call_price());
        }
    }
}

std::optional<market_data_bar> data_handler::get_next_market_data()
{
    if (current_data_index_ < ohlc_data_.size())
    {
        auto it = std::next(ohlc_data_.begin(), current_data_index_);
        current_data_index_++;
        return it->second;
    }
    return std::nullopt;
}