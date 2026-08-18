#include "csv_data_source.h"
#include "date_parse.h"
#include "market_series.h"
#include "market_types.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

bool load_csv_bars(const std::filesystem::path& path, IMarketSink& sink, LoadStats* stats)
{
	std::ifstream file(path);
	if (!file.good())
	{
		if (stats) stats->message = "CSV file error: " + path.string();
		return false;
	}

	std::string line;
	if (!std::getline(file, line))
	{
		if (stats) stats->message = "CSV empty: " + path.string();
		return false;
	}

	std::unordered_map<std::string, int> col_index;
	{
		std::stringstream hss(line);
		std::string col;
		int idx = 0;
		while (std::getline(hss, col, ','))
		{
			auto start = col.find_first_not_of(" \t\r\n");
			auto end = col.find_last_not_of(" \t\r\n");
			col_index[start == std::string::npos ? "" : col.substr(start, end - start + 1)] = idx++;
		}
	}

	for (const auto* name : {"open", "high", "low", "close"})
	{
		if (!col_index.count(name))
		{
			if (stats) stats->message = std::string("CSV missing required column: ") + name;
			return false;
		}
	}

	// Pre-reserve when sink is a MarketSeries
	if (auto* series = dynamic_cast<MarketSeries*>(&sink))
	{
		const auto fsz = std::filesystem::file_size(path);
		const size_t est_rows = (fsz > 0) ? (fsz / 60 + 1024) : 2'000'000;
		series->reserve_bars(est_rows);
	}

	std::size_t accepted = 0;
	std::size_t rejected = 0;

	while (std::getline(file, line))
	{
		if (line.empty()) continue;

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
				size_t idx = static_cast<size_t>(it->second);
				return (idx < fields.size()) ? std::string(fields[idx]) : std::string{};
			};

			Bar bar;
			bar.date = get("date");
			bar.symbol = get("symbol");

			std::string ot_s = get("open_time");
			if (!ot_s.empty())
			{
				try
				{
					const auto ms = std::stoll(ot_s);
					if (ms > 0)
					{
						bar.ts = std::chrono::system_clock::time_point{
							std::chrono::milliseconds{ms}};
					}
				}
				catch (...) {}
			}
			if (bar.ts == std::chrono::system_clock::time_point{})
			{
				if (auto tp = tt::date_parse::parse(bar.date))
					bar.ts = *tp;
			}

			std::string o_s = get("open");
			std::string h_s = get("high");
			std::string l_s = get("low");
			std::string c_s = get("close");
			std::string v_s = get("volume");

			bar.open = o_s.empty() ? 0.0 : std::stod(o_s);
			bar.high = h_s.empty() ? 0.0 : std::stod(h_s);
			bar.low = l_s.empty() ? 0.0 : std::stod(l_s);
			bar.close = c_s.empty() ? 0.0 : std::stod(c_s);
			// Integer volumes as-is; fractional base asset → * 1e8 (Binance klines).
			if (!v_s.empty())
			{
				try
				{
					std::size_t idx = 0;
					const long long iv = std::stoll(v_s, &idx);
					if (idx == v_s.size())
					{
						bar.volume = iv;
						bar.quantity_scale = 1;
					}
					else
						throw std::invalid_argument("fractional");
				}
				catch (...)
				{
					try
					{
						const double d = std::stod(v_s);
						if (std::isfinite(d) && d >= 0.0)
						{
							bar.volume = static_cast<int64_t>(std::llround(d * 1e8));
							bar.quantity_scale = 100'000'000ULL;
						}
					}
					catch (...) {}
				}
			}

			if (sink.on_bar(bar))
				++accepted;
			else
				++rejected;
		}
		catch (const std::exception&)
		{
			++rejected;
		}
	}

	if (stats)
	{
		stats->accepted = accepted;
		stats->rejected = rejected;
	}

	std::cout << "Loaded " << accepted << " records from CSV." << std::endl;
	return accepted > 0;
}

} // namespace

CsvDataSource::CsvDataSource(std::filesystem::path path)
	: path_(std::move(path)) {}

bool CsvDataSource::load_into(IMarketSink& sink, LoadStats* stats)
{
	return load_csv_bars(path_, sink, stats);
}

bool CsvDataSource::load_data(std::shared_ptr<data_handler> handler)
{
	if (!handler) return false;
	try
	{
		return load_into(*handler, nullptr);
	}
	catch (const std::exception& e)
	{
		std::cerr << "CSV load failed: " << e.what() << std::endl;
		return false;
	}
}
