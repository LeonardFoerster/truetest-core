#include "market_series.h"
#include "date_parse.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// The in-place sort application is deliberately the non-allocating commit
// phase after its index plans have been built. Keep that exception-safety
// boundary explicit if the domain records ever gain custom members.
static_assert(std::is_nothrow_move_constructible_v<std::string>);
static_assert(std::is_nothrow_move_assignable_v<std::string>);
static_assert(std::is_nothrow_move_constructible_v<Tick>);
static_assert(std::is_nothrow_move_assignable_v<Tick>);

std::chrono::system_clock::time_point parse_or_epoch(const std::string& date)
{
	if (auto tp = tt::date_parse::parse(date))
		return *tp;
	return {};
}

} // namespace

bool MarketSeries::validate_and_append_bar(std::string date, std::string symbol,
                                           std::chrono::system_clock::time_point ts,
                                           double o, double h, double l, double c, int64_t v,
                                           uint64_t quantity_scale)
{
	size_t row = bar_date_.size() + 1;

	if (!std::isfinite(o) || !std::isfinite(h) || !std::isfinite(l)
	    || !std::isfinite(c) || o <= 0 || h <= 0 || l <= 0 || c <= 0)
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
	if (quantity_scale == 0)
	{
		std::cerr << "  ! Row " << row << ": zero quantity scale, skipping\n";
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
	bar_quantity_scale_.emplace_back(quantity_scale);
	return true;
}

bool MarketSeries::on_bar(const Bar& bar)
{
	return validate_and_append_bar(bar.date, bar.symbol, bar.ts,
	                               bar.open, bar.high, bar.low, bar.close, bar.volume,
	                               bar.quantity_scale);
}

bool MarketSeries::on_tick(const Tick& tick)
{
	return add_tick(tick);
}

bool MarketSeries::load_into_queue(std::string date, std::string symbol,
                                   double o, double h, double l, double c, int64_t v,
                                   uint64_t quantity_scale)
{
	auto ts = parse_or_epoch(date);
	return validate_and_append_bar(std::move(date), std::move(symbol), ts,
	                               o, h, l, c, v, quantity_scale);
}

bool MarketSeries::add_tick(tick_record rec)
{
	if (!std::isfinite(rec.price) || rec.price <= 0)
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
	if (rec.quantity_scale == 0)
	{
		std::cerr << "  ! Tick: zero quantity scale, skipping\n";
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
	auto source_for_dest = sorted_bar_indices();
	apply_bar_permutation(source_for_dest);
}

void MarketSeries::sort_ticks_by_time()
{
	auto source_for_dest = sorted_tick_indices();
	apply_tick_permutation(source_for_dest);
}

void MarketSeries::sort_all_by_time()
{
	// Build both potentially allocating sort plans before changing either
	// backing store. Once those plans exist, the cycle applications below only
	// move standard library values in place.
	auto bar_source_for_dest = sorted_bar_indices();
	auto tick_source_for_dest = sorted_tick_indices();
	apply_bar_permutation(bar_source_for_dest);
	apply_tick_permutation(tick_source_for_dest);
}

std::vector<std::size_t> MarketSeries::sorted_bar_indices() const
{
	const std::size_t n = bar_symbol_.size();
	if (n < 2) return {};

	std::vector<std::size_t> source_for_dest(n);
	std::iota(source_for_dest.begin(), source_for_dest.end(), 0u);
	std::stable_sort(source_for_dest.begin(), source_for_dest.end(),
	                 [this](std::size_t a, std::size_t b) {
	                 if (bar_ts_[a] != bar_ts_[b])
		                 return bar_ts_[a] < bar_ts_[b];
	                 if (bar_symbol_[a] != bar_symbol_[b])
		                 return bar_symbol_[a] < bar_symbol_[b];
	                 return bar_date_[a] < bar_date_[b];
	                 });
	return source_for_dest;
}

std::vector<std::size_t> MarketSeries::sorted_tick_indices() const
{
	const std::size_t n = ticks_.size();
	if (n < 2) return {};

	std::vector<std::size_t> source_for_dest(n);
	std::iota(source_for_dest.begin(), source_for_dest.end(), 0u);
	std::stable_sort(source_for_dest.begin(), source_for_dest.end(),
	                 [this](std::size_t a, std::size_t b) {
	                 if (ticks_[a].timestamp != ticks_[b].timestamp)
		                 return ticks_[a].timestamp < ticks_[b].timestamp;
	                 return ticks_[a].symbol < ticks_[b].symbol;
	                 });
	return source_for_dest;
}

void MarketSeries::apply_bar_permutation(std::vector<std::size_t>& source_for_dest)
{
	for (std::size_t dest = 0; dest < source_for_dest.size(); ++dest)
	{
		if (source_for_dest[dest] == dest)
			continue;

		auto ts = std::move(bar_ts_[dest]);
		auto date = std::move(bar_date_[dest]);
		auto symbol = std::move(bar_symbol_[dest]);
		const double open = bar_open_[dest];
		const double high = bar_high_[dest];
		const double low = bar_low_[dest];
		const double close = bar_close_[dest];
		const int64_t volume = bar_volume_[dest];
		const uint64_t quantity_scale = bar_quantity_scale_[dest];

		std::size_t current = dest;
		while (source_for_dest[current] != dest)
		{
			const std::size_t source = source_for_dest[current];
			bar_ts_[current] = std::move(bar_ts_[source]);
			bar_date_[current] = std::move(bar_date_[source]);
			bar_symbol_[current] = std::move(bar_symbol_[source]);
			bar_open_[current] = bar_open_[source];
			bar_high_[current] = bar_high_[source];
			bar_low_[current] = bar_low_[source];
			bar_close_[current] = bar_close_[source];
			bar_volume_[current] = bar_volume_[source];
			bar_quantity_scale_[current] = bar_quantity_scale_[source];
			source_for_dest[current] = current;
			current = source;
		}

		bar_ts_[current] = std::move(ts);
		bar_date_[current] = std::move(date);
		bar_symbol_[current] = std::move(symbol);
		bar_open_[current] = open;
		bar_high_[current] = high;
		bar_low_[current] = low;
		bar_close_[current] = close;
		bar_volume_[current] = volume;
		bar_quantity_scale_[current] = quantity_scale;
		source_for_dest[current] = current;
	}
}

void MarketSeries::apply_tick_permutation(std::vector<std::size_t>& source_for_dest)
{
	for (std::size_t dest = 0; dest < source_for_dest.size(); ++dest)
	{
		if (source_for_dest[dest] == dest)
			continue;

		Tick saved = std::move(ticks_[dest]);
		std::size_t current = dest;
		while (source_for_dest[current] != dest)
		{
			const std::size_t source = source_for_dest[current];
			ticks_[current] = std::move(ticks_[source]);
			source_for_dest[current] = current;
			current = source;
		}
		ticks_[current] = std::move(saved);
		source_for_dest[current] = current;
	}
}

void MarketSeries::filter_window(
	std::optional<std::chrono::system_clock::time_point> from,
	std::optional<std::chrono::system_clock::time_point> to,
	const std::vector<std::string>& symbols)
{
	filter_appended_window({0, 0, validation_error_count_}, from, to, symbols);
}

void MarketSeries::filter_appended_window(
	AppendCheckpoint checkpoint,
	std::optional<std::chrono::system_clock::time_point> from,
	std::optional<std::chrono::system_clock::time_point> to,
	const std::vector<std::string>& symbols)
{
	if (checkpoint.bar_count > bar_count() || checkpoint.tick_count > tick_count())
		return;

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

	const std::size_t original_bar_count = bar_count();
	if (checkpoint.bar_count < original_bar_count)
	{
		std::size_t write = checkpoint.bar_count;
		for (std::size_t read = checkpoint.bar_count; read < original_bar_count; ++read)
		{
			if (!in_window(bar_ts_[read], bar_symbol_[read]))
				continue;

			if (write != read)
			{
				bar_ts_[write] = bar_ts_[read];
				bar_date_[write] = std::move(bar_date_[read]);
				bar_symbol_[write] = std::move(bar_symbol_[read]);
				bar_open_[write] = bar_open_[read];
				bar_high_[write] = bar_high_[read];
				bar_low_[write] = bar_low_[read];
				bar_close_[write] = bar_close_[read];
				bar_volume_[write] = bar_volume_[read];
				bar_quantity_scale_[write] = bar_quantity_scale_[read];
			}
			++write;
		}

		bar_ts_.resize(write);
		bar_date_.resize(write);
		bar_symbol_.resize(write);
		bar_open_.resize(write);
		bar_high_.resize(write);
		bar_low_.resize(write);
		bar_close_.resize(write);
		bar_volume_.resize(write);
		bar_quantity_scale_.resize(write);
	}

	const std::size_t original_tick_count = tick_count();
	if (checkpoint.tick_count < original_tick_count)
	{
		std::size_t write = checkpoint.tick_count;
		for (std::size_t read = checkpoint.tick_count; read < original_tick_count; ++read)
		{
			if (!in_window(ticks_[read].timestamp, ticks_[read].symbol))
				continue;
			if (write != read)
				ticks_[write] = std::move(ticks_[read]);
			++write;
		}
		ticks_.resize(write);
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
	v.quantity_scale = bar_quantity_scale_[i];
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
	bar_quantity_scale_.clear();
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
	bar_quantity_scale_.reserve(n);
}

void MarketSeries::reserve_ticks(std::size_t n)
{
	ticks_.reserve(n);
}

MarketSeries::AppendCheckpoint MarketSeries::append_checkpoint() const noexcept
{
	return {bar_count(), tick_count(), validation_error_count_};
}

void MarketSeries::rollback_appends(AppendCheckpoint checkpoint) noexcept
{
	// IMarketSource receives only the append-only IMarketSink interface, so a
	// checkpoint made immediately before load_into() can always be restored by
	// truncation. Reject an invalid external marker rather than accidentally
	// growing a column during an error path.
	if (checkpoint.bar_count > bar_count()
		|| checkpoint.tick_count > tick_count()
		|| checkpoint.validation_errors > validation_error_count_)
		return;

	bar_ts_.resize(checkpoint.bar_count);
	bar_date_.resize(checkpoint.bar_count);
	bar_symbol_.resize(checkpoint.bar_count);
	bar_open_.resize(checkpoint.bar_count);
	bar_high_.resize(checkpoint.bar_count);
	bar_low_.resize(checkpoint.bar_count);
	bar_close_.resize(checkpoint.bar_count);
	bar_volume_.resize(checkpoint.bar_count);
	bar_quantity_scale_.resize(checkpoint.bar_count);
	ticks_.resize(checkpoint.tick_count);
	validation_error_count_ = checkpoint.validation_errors;
}
