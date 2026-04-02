#include "tick_csv_data_source.h"
#include "data_handler.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <chrono>

bool TickCsvDataSource::load_data(std::shared_ptr<data_handler> handler)
{
    std::ifstream file(path_);
    if (!file.is_open())
    {
        std::cerr << "  ! Failed to open tick CSV: " << path_ << "\n";
        return false;
    }

    std::string line;
    // Skip header line
    if (!std::getline(file, line))
        return false;

    std::size_t count = 0;
    while (std::getline(file, line))
    {
        if (line.empty()) continue;

        std::istringstream ss(line);
        std::string token;

        // timestamp_ms
        if (!std::getline(ss, token, ',')) continue;
        int64_t ts_ms = std::stoll(token);

        // symbol
        std::string symbol;
        if (!std::getline(ss, symbol, ',')) continue;

        // price
        if (!std::getline(ss, token, ',')) continue;
        double price = std::stod(token);

        // quantity
        if (!std::getline(ss, token, ',')) continue;
        int64_t qty = std::stoll(token);

        // side (optional)
        data_tick_side side = data_tick_side::unknown;
        if (std::getline(ss, token, ','))
        {
            if (!token.empty())
            {
                char c = token[0];
                if (c == 'B' || c == 'b') side = data_tick_side::bid;
                else if (c == 'A' || c == 'a') side = data_tick_side::ask;
            }
        }

        auto timestamp = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(ts_ms));

        tick_record rec;
        rec.timestamp = timestamp;
        rec.symbol = symbol;
        rec.price = price;
        rec.quantity = qty;
        rec.side = side;
        if (handler->add_tick(std::move(rec)))
            ++count;
    }

    std::cout << "  Loaded " << count << " ticks from " << path_ << "\n";
    return count > 0;
}
