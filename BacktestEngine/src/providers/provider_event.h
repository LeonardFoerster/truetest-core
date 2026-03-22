#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace provider {

// Bar data (OHLCV) — from CSV, database, or aggregated from ticks.
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

// Single trade/tick — from exchange WebSocket, tick CSV, etc.
struct tick
{
	std::chrono::system_clock::time_point timestamp;
	std::string symbol;
	double price;
	int64_t quantity;
	uint8_t side;  // 0=bid, 1=ask, 2=unknown
};

// Level-2 orderbook snapshot — from exchange depth stream.
struct l2_snapshot
{
	std::chrono::system_clock::time_point timestamp;
	std::string symbol;
	struct level { double price; int64_t quantity; };
	std::vector<level> bids;
	std::vector<level> asks;
};

// Level-2 orderbook update — incremental depth update.
struct l2_update
{
	std::chrono::system_clock::time_point timestamp;
	std::string symbol;
	uint8_t side;  // 0=bid, 1=ask
	double price;
	int64_t new_quantity;  // 0 = level removed
};

// Status/control event — provider connection state, market status, etc.
struct status
{
	std::chrono::system_clock::time_point timestamp;
	std::string provider;
	std::string message;
	enum class kind : uint8_t { connected, disconnected, error, info };
	kind type;
};

// The unified provider event type.
using event = std::variant<bar, tick, l2_snapshot, l2_update, status>;

} // namespace provider
