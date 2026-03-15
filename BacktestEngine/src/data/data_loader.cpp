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
#include <unordered_map>
#include <vector>


void data_handler::load_into_queue(std::string date, std::string symbol, double o, double h, double l, double c, int64_t v)
{
        db_data_date.emplace_back(std::move(date));
        db_data_symbol.emplace_back(std::move(symbol));
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
    std::getline(file, line);

    std::unordered_map<std::string, int> col_index;
    {
        std::stringstream hss(line);
        std::string col;
        int idx = 0;
        while (std::getline(hss, col, ','))
        {
            // trim leading/trailing whitespace
            auto start = col.find_first_not_of(" \t\r\n");
            auto end   = col.find_last_not_of(" \t\r\n");
            col_index[start == std::string::npos ? "" : col.substr(start, end - start + 1)] = idx++;
        }
    }

    for (const auto& name : {"open", "high", "low", "close"})
    {
        if (!col_index.count(name))
            throw std::runtime_error("CSV missing required column: " + std::string(name));
    }

    while (std::getline(file, line))
    {
        if (line.empty()) continue;

        std::vector<std::string> tokens;
        {
            std::stringstream ss(line);
            std::string token;
            while (std::getline(ss, token, ','))
                tokens.emplace_back(std::move(token));
        }

        try
        {
            std::string date   = col_index.count("date")   ? tokens[col_index["date"]]   : "";
            std::string symbol = col_index.count("symbol") ? tokens[col_index["symbol"]] : "";
            double open        = std::stod(tokens[col_index["open"]]);
            double high        = std::stod(tokens[col_index["high"]]);
            double low         = std::stod(tokens[col_index["low"]]);
            double close       = std::stod(tokens[col_index["close"]]);
            int64_t volume     = col_index.count("volume") ? std::stoll(tokens[col_index["volume"]]) : 0;

            load_into_queue(date, symbol, open, high, low, close, volume);
        }
        catch (const std::exception&)
        {
        }
    }
}
