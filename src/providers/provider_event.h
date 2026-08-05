#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace provider {

struct bar
{
	std::string date;
	std::string symbol;
	double open;
	double high;
	double low;
	double close;
	int64_t volume;
};

struct tick
{
	std::chrono::system_clock::time_point timestamp;
	std::string symbol;
	double price;
	int64_t quantity;
	uint8_t side;

	// --- Footprint enrichment (footprint.md §2.1) - additive, optional. ---
	// Existing conversion (provider_convert.h to_tick_record/from_tick_record)
	// ignores these fields entirely, preserving strategy and execution
	// behavior unchanged. They are populated only by venues wired for the
	// opt-in footprint research tap (providers/data_bridge.h); all-zero
	// defaults mean "not populated" and must never be read as real data.
	uint64_t native_trade_id = 0;      // venue-native trade id; 0 = none
	int64_t  price_ticks = 0;          // exact integer price ticks; valid iff has_exact_decimal
	int64_t  base_qty_atoms = 0;       // exact integer base-qty atoms; valid iff has_exact_decimal
	bool     has_exact_decimal = false; // true once the parser populated the two fields above
};

struct l2_snapshot
{
	std::chrono::system_clock::time_point timestamp;
	std::string symbol;
	struct level { double price; int64_t quantity; };
	std::vector<level> bids;
	std::vector<level> asks;
};

struct l2_update
{
	std::chrono::system_clock::time_point timestamp;
	std::string symbol;
	uint8_t side;
	double price;
	int64_t new_quantity;
};

struct status
{
	std::chrono::system_clock::time_point timestamp;
	std::string provider;
	std::string message;
	enum class kind : uint8_t { connected, disconnected, error, info };
	kind type;
};

using event = std::variant<bar, tick, l2_snapshot, l2_update, status>;

}
