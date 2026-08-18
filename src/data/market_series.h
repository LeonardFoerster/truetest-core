#pragma once

// MarketSeries — format-independent market store (docs/internal/data-pipeline.md §5.4).
// Evolved from data_handler. Knows validation + SoA/AoS layout only — no CSV/Parquet/HTTP.

#include "data/market_sink.h"
#include "data/market_types.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
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
	                     double o, double h, double l, double c, int64_t v,
	                     uint64_t quantity_scale = 1);
	bool add_tick(tick_record rec);

	// ── Capacity / lifecycle ───────────────────────────────────────────────
	void reserve(std::size_t n);          // bars + ticks
	void reserve_bars(std::size_t n);
	void reserve_ticks(std::size_t n);
	void clear();                         // MC reuse: clear content, keep capacity
	void reset() { clear(); }             // legacy alias

	// Cold-path batch loaders can use this marker to fail atomically without
	// staging a second copy of a potentially large series. It is valid only
	// while the series has received append-only mutations since it was taken.
	struct AppendCheckpoint
	{
		std::size_t bar_count = 0;
		std::size_t tick_count = 0;
		std::size_t validation_errors = 0;
	};
	[[nodiscard]] AppendCheckpoint append_checkpoint() const noexcept;
	void rollback_appends(AppendCheckpoint checkpoint) noexcept;
	void filter_appended_window(
		AppendCheckpoint checkpoint,
		std::optional<std::chrono::system_clock::time_point> from,
		std::optional<std::chrono::system_clock::time_point> to,
		const std::vector<std::string>& symbols);

	// ── Ordering ───────────────────────────────────────────────────────────
	// Primary: bar_ts ascending; secondary: symbol. Falls back to date string
	// when timestamps are equal/default.
	void sort_bars_by_time();
	void sort_by_date() { sort_bars_by_time(); } // legacy alias
	// Sort ticks by timestamp ascending (stable). Call after multi-file or
	// out-of-order tick loads so the engine never sees a silently truncated tape.
	void sort_ticks_by_time();
	// Prepare both permutations before changing either store. Batch facades use
	// this when a load can contain bars and ticks and needs one rollback boundary.
	void sort_all_by_time();

	// DR-REPLAY-04: drop bars/ticks outside [from,to] and not in symbols
	// (empty symbols = keep all). Applied after load in DataWrapper.
	void filter_window(std::optional<std::chrono::system_clock::time_point> from,
	                   std::optional<std::chrono::system_clock::time_point> to,
	                   const std::vector<std::string>& symbols);

	// ── Read API (zero-extra-alloc views for engine batch loops) ───────────
	// BarView::symbol/date are string_view into SoA storage. Valid only until
	// the next mutating series operation (filter_window, clear, sort that
	// reorders strings, set_all_bar_symbols, further load). Do not store
	// BarView across those calls — use bar_symbol_at / bar_date_at for stable
	// string refs, or copy into std::string immediately (engine market_event).
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
		uint64_t quantity_scale = 1;
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
	                             double o, double h, double l, double c, int64_t v,
	                             uint64_t quantity_scale);
	std::vector<std::size_t> sorted_bar_indices() const;
	std::vector<std::size_t> sorted_tick_indices() const;
	void apply_bar_permutation(std::vector<std::size_t>& source_for_dest);
	void apply_tick_permutation(std::vector<std::size_t>& source_for_dest);

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
	std::vector<uint64_t> bar_quantity_scale_;

	// Tick AoS
	std::vector<Tick> ticks_;
};

// Migration typedef — one release/PR cycle (docs/internal/data-pipeline.md §4.3 / D-03).
using data_handler = MarketSeries;
