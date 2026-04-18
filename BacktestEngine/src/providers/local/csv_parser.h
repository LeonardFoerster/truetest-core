#pragma once

#include "providers/parser.h"
#include "data/data_handler.h"

#include <sstream>
#include <unordered_map>
#include <vector>
#include <string>
#include <chrono>
#include <cstdint>
#include <stdexcept>

#ifdef HAS_DEBUG
#include "debug/copy_tracker.h"
#endif


struct bar_record
#ifdef HAS_DEBUG
    : public debug::CopyTracker<bar_record>
#endif
{
	std::string date;
	std::string symbol;
	double open;
	double high;
	double low;
	double close;
	int64_t volume;
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

		std::vector<std::string> tokens;
		std::stringstream ss(line);
		std::string token;
		while (std::getline(ss, token, ','))
			tokens.emplace_back(std::move(token));

		try
		{
			bar_record rec;
			rec.date   = col_index_.count("date")   ? tokens.at(col_index_["date"])   : "";
			rec.symbol = col_index_.count("symbol") ? tokens.at(col_index_["symbol"]) : "";
			rec.open   = std::stod(tokens.at(col_index_["open"]));
			rec.high   = std::stod(tokens.at(col_index_["high"]));
			rec.low    = std::stod(tokens.at(col_index_["low"]));
			rec.close  = std::stod(tokens.at(col_index_["close"]));
			rec.volume = col_index_.count("volume") ? std::stoll(tokens.at(col_index_["volume"])) : 0;
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

		std::istringstream ss(line);
		std::string token;

		if (!std::getline(ss, token, ',')) return std::nullopt;
		int64_t ts_ms = std::stoll(token);

		std::string symbol;
		if (!std::getline(ss, symbol, ',')) return std::nullopt;

		if (!std::getline(ss, token, ',')) return std::nullopt;
		double price = std::stod(token);

		if (!std::getline(ss, token, ',')) return std::nullopt;
		int64_t qty = std::stoll(token);

		data_tick_side side = data_tick_side::unknown;
		if (std::getline(ss, token, ',') && !token.empty())
		{
			char c = token[0];
			if (c == 'B' || c == 'b') side = data_tick_side::bid;
			else if (c == 'A' || c == 'a') side = data_tick_side::ask;
		}

		auto timestamp = std::chrono::system_clock::time_point(
			std::chrono::milliseconds(ts_ms));

		tick_record rec;
		rec.timestamp = timestamp;
		rec.symbol = symbol;
		rec.price = price;
		rec.quantity = qty;
		rec.side = side;
		return rec;
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
