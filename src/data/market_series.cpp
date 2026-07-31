#include "market_series.h"
#include "date_parse.h"

#include <algorithm>
#include <iostream>
#include <numeric>
#include <utility>
#include <vector>

namespace {

std::chrono::system_clock::time_point parse_or_epoch(const std::string& date)
{
	if (auto tp = tt::date_parse::parse(date))
		return *tp;
	return {};
}

} // namespace

bool MarketSeries::validate_and_append_bar(std::string date, std::string symbol,
                                           std::chrono::system_clock::time_point ts,
                                           double o, double h, double l, double c, int64_t v)
{
	size_t row = bar_date_.size() + 1;

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

	if (ts == std::chrono::system_clock::time_point{} && !date.empty())
		ts = parse_or_epoch(date);

	bar_ts_.push_back(ts);
	bar_date_.emplace_back(std::move(date));
	bar_symbol_.emplace_back(std::move(symbol));
	bar_open_.emplace_back(o);
	bar_high_.emplace_back(h);
	bar_low_.emplace_back(l);
	bar_close_.emplace_back(c);
	bar_volume_.emplace_back(v);
	return true;
}

bool MarketSeries::on_bar(const Bar& bar)
{
	return validate_and_append_bar(bar.date, bar.symbol, bar.ts,
	                               bar.open, bar.high, bar.low, bar.close, bar.volume);
}

bool MarketSeries::on_tick(const Tick& tick)
{
	return add_tick(tick);
}

bool MarketSeries::load_into_queue(std::string date, std::string symbol,
                                   double o, double h, double l, double c, int64_t v)
{
	auto ts = parse_or_epoch(date);
	return validate_and_append_bar(std::move(date), std::move(symbol), ts, o, h, l, c, v);
}

bool MarketSeries::add_tick(tick_record rec)
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
	if (!ticks_.empty() && rec.timestamp < ticks_.back().timestamp)
	{
		std::cerr << "  ! Tick: non-monotonic timestamp, skipping\n";
		++validation_error_count_;
		return false;
	}

	ticks_.push_back(std::move(rec));
	return true;
}

void MarketSeries::sort_bars_by_time()
{
	const std::size_t n = bar_symbol_.size();
	if (n < 2) return;

	std::vector<std::size_t> idx(n);
	std::iota(idx.begin(), idx.end(), 0u);
	std::stable_sort(idx.begin(), idx.end(),
	                 [this](std::size_t a, std::size_t b) {
		                 if (bar_ts_[a] != bar_ts_[b])
			                 return bar_ts_[a] < bar_ts_[b];
		                 if (bar_symbol_[a] != bar_symbol_[b])
			                 return bar_symbol_[a] < bar_symbol_[b];
		                 return bar_date_[a] < bar_date_[b];
	                 });

	auto reorder_str = [&](std::vector<std::string>& v) {
		std::vector<std::string> out;
		out.reserve(n);
		for (auto i : idx) out.push_back(std::move(v[i]));
		v = std::move(out);
	};
	auto reorder_double = [&](std::vector<double>& v) {
		std::vector<double> out;
		out.reserve(n);
		for (auto i : idx) out.push_back(v[i]);
		v = std::move(out);
	};
	auto reorder_int = [&](std::vector<int64_t>& v) {
		std::vector<int64_t> out;
		out.reserve(n);
		for (auto i : idx) out.push_back(v[i]);
		v = std::move(out);
	};
	auto reorder_tp = [&](std::vector<std::chrono::system_clock::time_point>& v) {
		std::vector<std::chrono::system_clock::time_point> out;
		out.reserve(n);
		for (auto i : idx) out.push_back(v[i]);
		v = std::move(out);
	};

	reorder_tp(bar_ts_);
	reorder_str(bar_date_);
	reorder_str(bar_symbol_);
	reorder_double(bar_open_);
	reorder_double(bar_high_);
	reorder_double(bar_low_);
	reorder_double(bar_close_);
	reorder_int(bar_volume_);
}

MarketSeries::BarView MarketSeries::bar_at(std::size_t i) const
{
	BarView v;
	v.ts = bar_ts_[i];
	v.symbol = bar_symbol_[i];
	v.date = bar_date_[i];
	v.open = bar_open_[i];
	v.high = bar_high_[i];
	v.low = bar_low_[i];
	v.close = bar_close_[i];
	v.volume = bar_volume_[i];
	return v;
}

const Tick& MarketSeries::tick_at(std::size_t i) const
{
	return ticks_[i];
}

std::string MarketSeries::first_symbol() const
{
	if (bar_symbol_.empty()) return {};
	return bar_symbol_.front();
}

void MarketSeries::set_all_bar_symbols(const std::string& symbol)
{
	for (auto& s : bar_symbol_)
		s = symbol;
	if (bar_symbol_.empty())
		bar_symbol_.push_back(symbol);
}

void MarketSeries::clear()
{
	validation_error_count_ = 0;

	bar_ts_.clear();
	bar_date_.clear();
	bar_symbol_.clear();
	bar_open_.clear();
	bar_high_.clear();
	bar_low_.clear();
	bar_close_.clear();
	bar_volume_.clear();
	ticks_.clear();
}

void MarketSeries::reserve(std::size_t n)
{
	reserve_bars(n);
	reserve_ticks(n);
}

void MarketSeries::reserve_bars(std::size_t n)
{
	bar_ts_.reserve(n);
	bar_date_.reserve(n);
	bar_symbol_.reserve(n);
	bar_open_.reserve(n);
	bar_high_.reserve(n);
	bar_low_.reserve(n);
	bar_close_.reserve(n);
	bar_volume_.reserve(n);
}

void MarketSeries::reserve_ticks(std::size_t n)
{
	ticks_.reserve(n);
}
