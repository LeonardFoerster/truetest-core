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
#include <filesystem>
#include <string_view>


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

    std::vector<std::size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0u);
    std::stable_sort(idx.begin(), idx.end(),
                     [this](std::size_t a, std::size_t b) {
                         return db_data_date[a] < db_data_date[b];
                     });

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

    // Performance: rough pre-reserve based on file size heuristic.
    // Combined with the fast field walking below and fast_stoll this makes
    // the legacy path also usable for reasonably large files without instant pain.
    const auto fsz = std::filesystem::file_size(path);
    const size_t est_rows = (fsz > 0) ? (fsz / 60 + 1024) : 2'000'000;
    reserve(est_rows);

    while (std::getline(file, line))
    {
        if (line.empty()) continue;

        // Same lightweight field extraction as the fast CsvBarParser.
        std::vector<std::string_view> fields;
        fields.reserve(8);
        std::string_view sv(line);
        size_t pos = 0;
        while (pos <= sv.size())
        {
            size_t next = sv.find(',', pos);
            if (next == std::string_view::npos) next = sv.size();
            fields.emplace_back(sv.substr(pos, next - pos));
            if (next == sv.size()) break;
            pos = next + 1;
        }

        try
        {
            auto get = [&](const char* name) -> std::string {
                auto it = col_index.find(name);
                if (it == col_index.end()) return {};
                size_t idx = it->second;
                return (idx < fields.size()) ? std::string(fields[idx]) : std::string{};
            };

            std::string date   = get("date");
            std::string symbol = get("symbol");
            std::string o_s    = get("open");
            std::string h_s    = get("high");
            std::string l_s    = get("low");
            std::string c_s    = get("close");
            std::string v_s    = get("volume");

            double open    = o_s.empty() ? 0.0 : std::stod(o_s);
            double high    = h_s.empty() ? 0.0 : std::stod(h_s);
            double low     = l_s.empty() ? 0.0 : std::stod(l_s);
            double close   = c_s.empty() ? 0.0 : std::stod(c_s);
            int64_t volume = 0;
            if (!v_s.empty())
            {
                // Legacy path: we accept a slightly slower stoll here.
                // The hot path (CsvBarParser + DataBridge for --provider local) already uses fast_stoll.
                try { volume = std::stoll(v_s); } catch (...) {}
            }

            load_into_queue(date, symbol, open, high, low, close, volume);
        }
        catch (const std::exception&)
        {
        }
    }
}

// Phase A (MC reuse): clears all data so the handler can be reused for the next trial.
void data_handler::reset()
{
    current_csv_row_index_ = 0;
    validation_error_count_ = 0;

    db_data_date.clear();
    db_data_symbol.clear();
    db_data_open_value.clear();
    db_data_high_value.clear();
    db_data_low_value.clear();
    db_data_close_value.clear();
    db_data_volume_value.clear();

    tick_data.clear();
}

// Performance: pre-reserve to avoid repeated realloc + memmove when loading
// large CSVs (1.7M+ rows over multiple years is common). This is one of the
// biggest cheap wins for repeated backtests on multi-year bar data.
void data_handler::reserve(std::size_t n)
{
    db_data_date.reserve(n);
    db_data_symbol.reserve(n);
    db_data_open_value.reserve(n);
    db_data_high_value.reserve(n);
    db_data_low_value.reserve(n);
    db_data_close_value.reserve(n);
    db_data_volume_value.reserve(n);
    tick_data.reserve(n);
}
