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
