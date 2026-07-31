#pragma once

#include "providers/parser.h"
#include "data/data_handler.h"

#include <sstream>
#include <unordered_map>
#include <vector>
#include <string>
#include <string_view>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <charconv>

#ifdef HAS_DEBUG
#include "debug/copy_tracker.h"
#endif

// Performance helpers for large CSVs (1.7M+ rows common for multi-year bar data).
// Goal: reduce the dominant cost when backtesting 4+ years of bars.
// - std::from_chars for integers (volume, timestamps, qty) – zero-alloc, fast.
// - Lightweight string_view field walking instead of vector<string> + stringstream per row.
// - Caller is expected to call data_handler::reserve() before loading.
namespace tt::csv {

// Base-asset volume scale for fractional exchange quantities (matches Binance kline path).
// Integer-only volume fields (legacy equity CSVs) are stored as-is without scaling.
inline constexpr double kVolumeScale = 1e8;

inline std::int64_t fast_stoll(std::string_view sv)
{
    std::int64_t v = 0;
    auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
    if (ec != std::errc{} || p != sv.data() + sv.size())
        throw std::invalid_argument("fast_stoll failed");
    return v;
}

// Integer volumes pass through; fractional (e.g. "246.092") → llround(v * 1e8).
inline std::int64_t parse_bar_volume(std::string_view sv)
{
    if (sv.empty()) return 0;
    try {
        return fast_stoll(sv);
    } catch (...) {
        try {
            const double d = std::stod(std::string(sv));
            if (!std::isfinite(d) || d < 0.0) return 0;
            return static_cast<std::int64_t>(std::llround(d * kVolumeScale));
        } catch (...) {
            return 0;
        }
    }
}

} // namespace tt::csv


struct bar_record
#ifdef HAS_DEBUG
    : public debug::CopyTracker<bar_record>
#endif
{
	std::string date;
	std::string symbol;
	double open = 0;
	double high = 0;
	double low = 0;
	double close = 0;
	int64_t volume = 0;
	// Epoch milliseconds from open_time (Binance kline CSV). 0 = unset → use date.
	int64_t open_time_ms = 0;
};

class CsvBarParser : public IDataParser<bar_record>
{
public:
	bool parse_header(const std::string& line) override
	{
		std::stringstream ss(line);
		std::string col;
		int idx = 0;

		while (std::getline(ss, col, ','))
		{
			auto start = col.find_first_not_of(" \t\r\n");
			auto end   = col.find_last_not_of(" \t\r\n");
			std::string trimmed = (start == std::string::npos)
				? ""
				: col.substr(start, end - start + 1);
			col_index_[trimmed] = idx++;
		}

		for (const auto& name : {"open", "high", "low", "close"})
		{
			if (!col_index_.count(name))
			{
				std::cerr << "CsvBarParser: missing required column: " << name << "\n";
				return false;
			}
		}
		return true;
	}

	std::optional<bar_record> parse_record(const std::string& line) override
	{
		if (line.empty()) return std::nullopt;

		// Performance: avoid per-line vector<string> + stringstream churn for
		// every row (this was one of the biggest costs at 1.7M+ rows).
		// We walk the line once with find and only materialise the fields we need.
		try
		{
			bar_record rec;
			std::string_view sv(line);

			// Very lightweight field walker – enough for typical OHLCV CSVs.
			// Keeps the existing column-index logic for flexibility.
			std::vector<std::string_view> fields;
			fields.reserve(8);
			size_t pos = 0;
			while (pos <= sv.size())
			{
				size_t next = sv.find(',', pos);
				if (next == std::string_view::npos) next = sv.size();
				fields.emplace_back(sv.substr(pos, next - pos));
				if (next == sv.size()) break;
				pos = next + 1;
			}

			auto get_field = [&](const char* name) -> std::string_view {
				auto it = col_index_.find(name);
				if (it == col_index_.end()) return {};
				size_t idx = it->second;
				return (idx < fields.size()) ? fields[idx] : std::string_view{};
			};

			auto date_sv   = get_field("date");
			auto sym_sv    = get_field("symbol");
			auto open_sv   = get_field("open");
			auto high_sv   = get_field("high");
			auto low_sv    = get_field("low");
			auto close_sv  = get_field("close");
			auto vol_sv    = get_field("volume");
			auto ot_sv     = get_field("open_time");

			rec.date   = std::string(date_sv);
			rec.symbol = std::string(sym_sv);

			// Fast path for integers, stod kept for doubles for portability.
			rec.open   = open_sv.empty()  ? 0.0 : std::stod(std::string(open_sv));
			rec.high   = high_sv.empty()  ? 0.0 : std::stod(std::string(high_sv));
			rec.low    = low_sv.empty()   ? 0.0 : std::stod(std::string(low_sv));
			rec.close  = close_sv.empty() ? 0.0 : std::stod(std::string(close_sv));
			rec.volume = tt::csv::parse_bar_volume(vol_sv);

			if (!ot_sv.empty())
			{
				try { rec.open_time_ms = tt::csv::fast_stoll(ot_sv); }
				catch (...) { rec.open_time_ms = 0; }
			}

			return rec;
		}
		catch (const std::exception&)
		{
			return std::nullopt;
		}
	}

private:
	std::unordered_map<std::string, int> col_index_;
};



class CsvTickParser : public IDataParser<tick_record>
{
public:
	std::optional<tick_record> parse_record(const std::string& line) override
	{
		if (line.empty()) return std::nullopt;

		try
		{
			// Same performance improvements as CsvBarParser:
			// reduce per-line allocations and use fast integer parsing.
			std::string_view sv(line);

			std::vector<std::string_view> fields;
			fields.reserve(6);
			size_t pos = 0;
			while (pos <= sv.size())
			{
				size_t next = sv.find(',', pos);
				if (next == std::string_view::npos) next = sv.size();
				fields.emplace_back(sv.substr(pos, next - pos));
				if (next == sv.size()) break;
				pos = next + 1;
			}

			if (fields.size() < 4) return std::nullopt;

			int64_t ts_ms = 0;
			try { ts_ms = tt::csv::fast_stoll(fields[0]); }
			catch (...) { return std::nullopt; }

			std::string symbol(fields[1]);

			double price = 0.0;
			try { price = std::stod(std::string(fields[2])); }
			catch (...) { return std::nullopt; }

			int64_t qty = 0;
			try { qty = tt::csv::fast_stoll(fields[3]); }
			catch (...) { return std::nullopt; }

			data_tick_side side = data_tick_side::unknown;
			if (fields.size() >= 5 && !fields[4].empty())
			{
				char c = fields[4][0];
				if (c == 'B' || c == 'b') side = data_tick_side::bid;
				else if (c == 'A' || c == 'a') side = data_tick_side::ask;
			}

			auto timestamp = std::chrono::system_clock::time_point(
				std::chrono::milliseconds(ts_ms));

			tick_record rec;
			rec.timestamp = timestamp;
			rec.symbol = std::move(symbol);
			rec.price = price;
			rec.quantity = qty;
			rec.side = side;
			return rec;
		}
		catch (const std::exception&)
		{
			return std::nullopt;
		}
	}
};



inline void bar_record_sink(const bar_record& rec, std::shared_ptr<data_handler> handler)
{
	handler->load_into_queue(rec.date, rec.symbol, rec.open, rec.high, rec.low, rec.close, rec.volume);
}

inline void tick_record_sink(const tick_record& rec, std::shared_ptr<data_handler> handler)
{
	handler->add_tick(rec);
}
