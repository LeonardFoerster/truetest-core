#pragma once

#include "provider_event.h"
#include "data/market_types.h"
#include "local/csv_parser.h"

#include <chrono>

namespace provider {

inline bar_record to_bar_record(const bar& b)
{
	bar_record rec;
	rec.date   = b.date;
	rec.symbol = b.symbol;
	rec.open   = b.open;
	rec.high   = b.high;
	rec.low    = b.low;
	rec.close  = b.close;
	rec.volume = b.volume;
	rec.quantity_scale = b.quantity_scale;
	return rec;
}

inline tick_record to_tick_record(const tick& t)
{
	tick_record rec;
	rec.timestamp = t.timestamp;
	rec.symbol    = t.symbol;
	rec.price     = t.price;
	rec.quantity  = t.quantity;
	rec.side      = static_cast<data_tick_side>(t.side);
	rec.quantity_scale = t.quantity_scale;
	return rec;
}

inline bar from_bar_record(const bar_record& r)
{
	bar out{r.date, r.symbol, r.open, r.high, r.low, r.close, r.volume};
	out.quantity_scale = r.quantity_scale;
	return out;
}

inline tick from_tick_record(const tick_record& r)
{
	tick out{r.timestamp, r.symbol, r.price, r.quantity,
	         static_cast<uint8_t>(r.side)};
	out.quantity_scale = r.quantity_scale;
	return out;
}

// Domain Bar helpers (docs/internal/data-pipeline.md D-01)
inline Bar to_domain_bar(const bar& b)
{
	Bar out;
	out.date = b.date;
	out.symbol = b.symbol;
	out.open = b.open;
	out.high = b.high;
	out.low = b.low;
	out.close = b.close;
	out.volume = b.volume;
	out.quantity_scale = b.quantity_scale;
	return out;
}

inline Bar to_domain_bar(const bar_record& r)
{
	Bar out;
	out.date = r.date;
	out.symbol = r.symbol;
	out.open = r.open;
	out.high = r.high;
	out.low = r.low;
	out.close = r.close;
	out.volume = r.volume;
	out.quantity_scale = r.quantity_scale;
	if (r.open_time_ms > 0)
	{
		out.ts = std::chrono::system_clock::time_point{
			std::chrono::milliseconds{r.open_time_ms}};
	}
	return out;
}

}
