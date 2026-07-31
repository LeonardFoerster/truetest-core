#include "tick_csv_data_source.h"
#include "market_series.h"
#include "market_types.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

bool TickCsvDataSource::load_into(IMarketSink& sink, LoadStats* stats)
{
	std::ifstream file(path_);
	if (!file.is_open())
	{
		std::cerr << "  ! Failed to open tick CSV: " << path_ << "\n";
		if (stats) stats->message = "Failed to open tick CSV: " + path_;
		return false;
	}

	std::string line;
	if (!std::getline(file, line))
	{
		if (stats) stats->message = "Tick CSV empty: " + path_;
		return false;
	}

	if (auto* series = dynamic_cast<MarketSeries*>(&sink))
		series->reserve_ticks(4096);

	std::size_t accepted = 0;
	std::size_t rejected = 0;

	while (std::getline(file, line))
	{
		if (line.empty()) continue;

		std::istringstream ss(line);
		std::string token;

		if (!std::getline(ss, token, ',')) continue;
		int64_t ts_ms = 0;
		try { ts_ms = std::stoll(token); } catch (...) { ++rejected; continue; }

		std::string symbol;
		if (!std::getline(ss, symbol, ',')) { ++rejected; continue; }

		if (!std::getline(ss, token, ',')) { ++rejected; continue; }
		double price = 0;
		try { price = std::stod(token); } catch (...) { ++rejected; continue; }

		if (!std::getline(ss, token, ',')) { ++rejected; continue; }
		int64_t qty = 0;
		try { qty = std::stoll(token); } catch (...) { ++rejected; continue; }

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

		Tick rec;
		rec.timestamp = std::chrono::system_clock::time_point(
			std::chrono::milliseconds(ts_ms));
		rec.symbol = std::move(symbol);
		rec.price = price;
		rec.quantity = qty;
		rec.side = side;

		if (sink.on_tick(rec))
			++accepted;
		else
			++rejected;
	}

	if (stats)
	{
		stats->accepted = accepted;
		stats->rejected = rejected;
	}

	std::cout << "  Loaded " << accepted << " ticks from " << path_ << "\n";
	return accepted > 0;
}

bool TickCsvDataSource::load_data(std::shared_ptr<data_handler> handler)
{
	if (!handler) return false;
	return load_into(*handler, nullptr);
}
