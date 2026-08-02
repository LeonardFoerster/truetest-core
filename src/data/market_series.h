#pragma once

// MarketSeries — format-independent market store (docs/internal/data-pipeline.md §5.4).
// Evolved from data_handler. Knows validation + SoA/AoS layout only — no CSV/Parquet/HTTP.

#include "data/market_sink.h"
#include "data/market_types.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class MarketSeries final : public IMarketSink
{
public:
	MarketSeries() = default;

	// ── IMarketSink ────────────────────────────────────────────────────────
	bool on_bar(const Bar& bar) override;
	bool on_tick(const Tick& tick) override;

	// ── Legacy write helpers (MC / tests / migration) ──────────────────────
	// date string is parsed once into bar_ts_ (not re-parsed on every engine bar).
	bool load_into_queue(std::string date, std::string symbol,
	                     double o, double h, double l, double c, int64_t v);
	bool add_tick(tick_record rec);

	// ── Capacity / lifecycle ───────────────────────────────────────────────
	void reserve(std::size_t n);          // bars + ticks
	void reserve_bars(std::size_t n);
	void reserve_ticks(std::size_t n);
	void clear();                         // MC reuse: clear content, keep capacity
	void reset() { clear(); }             // legacy alias

	// ── Ordering ───────────────────────────────────────────────────────────
	// Primary: bar_ts ascending; secondary: symbol. Falls back to date string
	// when timestamps are equal/default.
	void sort_bars_by_time();
	void sort_by_date() { sort_bars_by_time(); } // legacy alias

	// ── Read API (zero-extra-alloc views for engine batch loops) ───────────
	struct BarView
	{
		std::chrono::system_clock::time_point ts{};
		std::string_view symbol;
		std::string_view date;
		double open = 0;
		double high = 0;
		double low = 0;
		double close = 0;
		int64_t volume = 0;
	};

	std::size_t bar_count() const noexcept { return bar_symbol_.size(); }
	std::size_t tick_count() const noexcept { return ticks_.size(); }
	bool empty() const noexcept { return bar_symbol_.empty() && ticks_.empty(); }
	bool has_bar_data() const noexcept { return !bar_symbol_.empty(); }
	bool has_tick_data() const noexcept { return !ticks_.empty(); }

	BarView bar_at(std::size_t i) const;
	const Tick& tick_at(std::size_t i) const;

	// Stable string refs for engine market_event construction (no temp string).
	const std::string& bar_symbol_at(std::size_t i) const { return bar_symbol_[i]; }
	const std::string& bar_date_at(std::size_t i) const { return bar_date_[i]; }

	// Convenience for engine/config (first bar symbol, or empty).
	std::string first_symbol() const;
	// switch_symbol: replace every bar symbol (legacy engine helper).
	void set_all_bar_symbols(const std::string& symbol);

	std::size_t validation_errors() const noexcept { return validation_error_count_; }

private:
	bool validate_and_append_bar(std::string date, std::string symbol,
	                             std::chrono::system_clock::time_point ts,
	                             double o, double h, double l, double c, int64_t v);

	size_t validation_error_count_ = 0;

	// Bar SoA (private — no format knowledge)
	std::vector<std::chrono::system_clock::time_point> bar_ts_;
	std::vector<std::string> bar_date_;
	std::vector<std::string> bar_symbol_;
	std::vector<double> bar_open_;
	std::vector<double> bar_high_;
	std::vector<double> bar_low_;
	std::vector<double> bar_close_;
	std::vector<int64_t> bar_volume_;

	// Tick AoS
	std::vector<Tick> ticks_;
};

// Migration typedef — one release/PR cycle (docs/internal/data-pipeline.md §4.3 / D-03).
using data_handler = MarketSeries;
