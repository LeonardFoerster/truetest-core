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
	uint64_t quantity_scale = 1;
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
	// Scale of quantity when the exact-decimal enrichment is unavailable.
	// Appended to preserve existing aggregate initializers.
	uint64_t quantity_scale = 1;
};

struct l2_snapshot
{
	std::chrono::system_clock::time_point timestamp;
	std::string symbol;
	struct level { double price; int64_t quantity; };
	std::vector<level> bids;
	std::vector<level> asks;
	uint64_t quantity_scale = 1;
};

struct l2_update
{
	std::chrono::system_clock::time_point timestamp;
	std::string symbol;
	uint8_t side;
	double price;
	int64_t new_quantity;
	uint64_t quantity_scale = 1;
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
