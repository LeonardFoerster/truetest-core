#include "market_series.h"
#include "date_parse.h"

#include <algorithm>
#include <iostream>
#include <numeric>
#include <unordered_set>
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
	// Accept out-of-order ticks; callers sort via sort_ticks_by_time() after load
	// (DR-03: never silently drop OOO rows without a chance to reorder).
	if (!ticks_.empty() && rec.timestamp < ticks_.back().timestamp)
	{
		std::cerr << "  ! Tick: non-monotonic timestamp (will sort after load)\n";
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

void MarketSeries::sort_ticks_by_time()
{
	if (ticks_.size() < 2) return;
	std::stable_sort(ticks_.begin(), ticks_.end(),
	                 [](const Tick& a, const Tick& b) {
		                 if (a.timestamp != b.timestamp)
			                 return a.timestamp < b.timestamp;
		                 return a.symbol < b.symbol;
	                 });
}

void MarketSeries::filter_window(
	std::optional<std::chrono::system_clock::time_point> from,
	std::optional<std::chrono::system_clock::time_point> to,
	const std::vector<std::string>& symbols)
{
	std::unordered_set<std::string> sym_set(symbols.begin(), symbols.end());
	const bool filter_sym = !sym_set.empty();

	auto in_window = [&](std::chrono::system_clock::time_point ts,
	                     const std::string& sym) {
		if (filter_sym && sym_set.count(sym) == 0)
			return false;
		// Default/empty timestamps: keep when no from/to bound applies to them
		// only if neither bound is set; otherwise drop unparseable history.
		if (from && ts != std::chrono::system_clock::time_point{} && ts < *from)
			return false;
		if (from && ts == std::chrono::system_clock::time_point{})
			return false;
		if (to && ts != std::chrono::system_clock::time_point{} && ts > *to)
			return false;
		if (to && ts == std::chrono::system_clock::time_point{})
			return false;
		return true;
	};

	if (!bar_symbol_.empty())
	{
		const std::size_t n = bar_symbol_.size();
		std::vector<std::size_t> keep;
		keep.reserve(n);
		for (std::size_t i = 0; i < n; ++i)
		{
			if (in_window(bar_ts_[i], bar_symbol_[i]))
				keep.push_back(i);
		}
		if (keep.size() != n)
		{
			auto pick_str = [&](std::vector<std::string>& v) {
				std::vector<std::string> out;
				out.reserve(keep.size());
				for (auto i : keep) out.push_back(std::move(v[i]));
				v = std::move(out);
			};
			auto pick_d = [&](std::vector<double>& v) {
				std::vector<double> out;
				out.reserve(keep.size());
				for (auto i : keep) out.push_back(v[i]);
				v = std::move(out);
			};
			auto pick_i = [&](std::vector<int64_t>& v) {
				std::vector<int64_t> out;
				out.reserve(keep.size());
				for (auto i : keep) out.push_back(v[i]);
				v = std::move(out);
			};
			auto pick_tp = [&](std::vector<std::chrono::system_clock::time_point>& v) {
				std::vector<std::chrono::system_clock::time_point> out;
				out.reserve(keep.size());
				for (auto i : keep) out.push_back(v[i]);
				v = std::move(out);
			};
			pick_tp(bar_ts_);
			pick_str(bar_date_);
			pick_str(bar_symbol_);
			pick_d(bar_open_);
			pick_d(bar_high_);
			pick_d(bar_low_);
			pick_d(bar_close_);
			pick_i(bar_volume_);
		}
	}

	if (!ticks_.empty())
	{
		std::vector<Tick> kept;
		kept.reserve(ticks_.size());
		for (auto& t : ticks_)
		{
			if (in_window(t.timestamp, t.symbol))
				kept.push_back(std::move(t));
		}
		ticks_ = std::move(kept);
	}
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
