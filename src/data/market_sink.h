#pragma once

// IMarketSink — append/process port for market records (docs/data.md §5.2).
// Sources depend only on this + market_types; they must not include market_series.

#include "data/market_types.h"

#include <cstddef>
#include <string>

struct LoadStats
{
	std::size_t accepted = 0;
	std::size_t rejected = 0;
	std::string message;
};

class IMarketSink
{
public:
	virtual ~IMarketSink() = default;

	// return false = reject record (validation); sources continue unless hard-fail policy
	virtual bool on_bar(const Bar& bar) = 0;
	virtual bool on_tick(const Tick& tick) = 0;
};
