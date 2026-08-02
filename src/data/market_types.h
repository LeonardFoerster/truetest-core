#pragma once

// Canonical market domain records (docs/internal/data-pipeline.md §5.1).
// No I/O, no venue headers, no engine includes.
// Note: do NOT name the side enum `tick_side` — that collides with core/event.h.

#include <chrono>
#include <cstdint>
#include <string>

#ifdef HAS_DEBUG
#include "debug/copy_tracker.h"
#endif

enum class data_tick_side : uint8_t
{
	bid = 0,
	ask = 1,
	unknown = 2
};

struct Bar
{
	std::chrono::system_clock::time_point ts{};
	std::string symbol;
	// Original date token from CSV/API (optional; empty when ts was primary).
	std::string date;
	double open = 0;
	double high = 0;
	double low = 0;
	double close = 0;
	int64_t volume = 0;
};

struct Tick
#ifdef HAS_DEBUG
	: public debug::CopyTracker<Tick>
#endif
{
	std::chrono::system_clock::time_point timestamp{};
	std::string symbol;
	double price = 0;
	int64_t quantity = 0;
	data_tick_side side = data_tick_side::unknown;
};

// Legacy name kept for one migration cycle (parsers, DataBridge, engine ticks).
using tick_record = Tick;
