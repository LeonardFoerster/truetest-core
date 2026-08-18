#pragma once

#include "providers/parser.h"
#include "data/data_handler.h"
#include "data/quantity_scale.h"

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

inline constexpr std::uint64_t kIntegerQuantityScale = 1;
inline constexpr std::uint64_t kFractionalQuantityScale = 100'000'000ULL;

inline std::int64_t fast_stoll(std::string_view sv)
{
    std::int64_t v = 0;
    auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
    if (ec != std::errc{} || p != sv.data() + sv.size())
        throw std::invalid_argument("fast_stoll failed");
    return v;
}

inline bool parse_nonnegative_quantity(std::string_view sv,
                                       std::int64_t& out,
                                       std::uint64_t& quantity_scale)
{
    out = 0;
    quantity_scale = kIntegerQuantityScale;
    if (sv.empty()) return true;

    try {
        out = fast_stoll(sv);
        return out >= 0;
    } catch (...) {
        try {
            const std::string token(sv);
            std::size_t parsed = 0;
            const double value = std::stod(token, &parsed);
            if (parsed != token.size()
                || !tt::quantity_scale::from_base_nonnegative(
                    value, kFractionalQuantityScale, out))
                return false;
            quantity_scale = kFractionalQuantityScale;
            return true;
        } catch (...) {
            return false;
        }
    }
}

// Integer volumes pass through; fractional (e.g. "246.092") use 1e8 atoms.
inline std::int64_t parse_bar_volume(std::string_view sv,
                                     std::uint64_t* quantity_scale = nullptr)
{
    std::int64_t out = 0;
    std::uint64_t scale = kIntegerQuantityScale;
    (void)parse_nonnegative_quantity(sv, out, scale);
    if (quantity_scale) *quantity_scale = scale;
    return out;
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
	uint64_t quantity_scale = 1;
};

class CsvBarParser : public IDataParser<bar_record>
{
public:
	bool header_frame_contains_records() const override { return false; }
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
			if (!tt::csv::parse_nonnegative_quantity(
			        vol_sv, rec.volume, rec.quantity_scale))
				return std::nullopt;

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
	bool header_frame_contains_records() const override { return false; }
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
			std::uint64_t quantity_scale = 1;
			if (!tt::csv::parse_nonnegative_quantity(
			        fields[3], qty, quantity_scale))
				return std::nullopt;

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
			rec.quantity_scale = quantity_scale;
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
	handler->load_into_queue(rec.date, rec.symbol, rec.open, rec.high, rec.low,
	                         rec.close, rec.volume, rec.quantity_scale);
}

inline void tick_record_sink(const tick_record& rec, std::shared_ptr<data_handler> handler)
{
	handler->add_tick(rec);
}
