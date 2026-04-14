#include "data_handler.h"

#include <algorithm>
#include <numeric>
#include <sstream>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <vector>


bool data_handler::load_into_queue(std::string date, std::string symbol, double o, double h, double l, double c, int64_t v)
{
        size_t row = db_data_date.size() + 1;

        if (o <= 0 || h <= 0 || l <= 0 || c <= 0)
        {
            std::cerr << "  ! Row " << row << ": non-positive price (o=" << o
                      << " h=" << h << " l=" << l << " c=" << c << "), skipping\n";
            ++validation_error_count_;
            return false;
        }
        if (h < l)
        {
            std::cerr << "  ! Row " << row << ": high (" << h << ") < low (" << l << "), skipping\n";
            ++validation_error_count_;
            return false;
        }
        if (v < 0)
        {
            std::cerr << "  ! Row " << row << ": negative volume (" << v << "), skipping\n";
            ++validation_error_count_;
            return false;
        }

        db_data_date.emplace_back(std::move(date));
        db_data_symbol.emplace_back(std::move(symbol));
        db_data_open_value.emplace_back(o);
        db_data_high_value.emplace_back(h);
        db_data_low_value.emplace_back(l);
        db_data_close_value.emplace_back(c);
        db_data_volume_value.emplace_back(v);
        return true;
}

bool data_handler::add_tick(tick_record rec)
{
        if (rec.price <= 0)
        {
            std::cerr << "  ! Tick: non-positive price (" << rec.price << "), skipping\n";
            ++validation_error_count_;
            return false;
        }
        if (rec.quantity <= 0)
        {
            std::cerr << "  ! Tick: non-positive quantity (" << rec.quantity << "), skipping\n";
            ++validation_error_count_;
            return false;
        }
        if (!tick_data.empty() && rec.timestamp < tick_data.back().timestamp)
        {
            std::cerr << "  ! Tick: non-monotonic timestamp, skipping\n";
            ++validation_error_count_;
            return false;
        }

        tick_data.push_back(std::move(rec));
        return true;
}

void data_handler::sort_by_date()
{
    const std::size_t n = db_data_date.size();
    if (n < 2) return;

    // Build index permutation sorted by date (stable so same-date rows keep load order)
    std::vector<std::size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0u);
    std::stable_sort(idx.begin(), idx.end(),
                     [this](std::size_t a, std::size_t b) {
                         return db_data_date[a] < db_data_date[b];
                     });

    // Apply permutation to all parallel vectors
    auto reorder_str    = [&](std::vector<std::string>& v) {
        std::vector<std::string> out; out.reserve(n);
        for (auto i : idx) out.push_back(std::move(v[i]));
        v = std::move(out);
    };
    auto reorder_double = [&](std::vector<double>& v) {
        std::vector<double> out; out.reserve(n);
        for (auto i : idx) out.push_back(v[i]);
        v = std::move(out);
    };
    auto reorder_int    = [&](std::vector<int64_t>& v) {
        std::vector<int64_t> out; out.reserve(n);
        for (auto i : idx) out.push_back(v[i]);
        v = std::move(out);
    };
    reorder_str(db_data_date);
    reorder_str(db_data_symbol);
    reorder_double(db_data_open_value);
    reorder_double(db_data_high_value);
    reorder_double(db_data_low_value);
    reorder_double(db_data_close_value);
    reorder_int(db_data_volume_value);
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
