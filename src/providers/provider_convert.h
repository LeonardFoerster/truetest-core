#pragma once

#include "provider_event.h"
#include "data/data_handler.h"
#include "local/csv_parser.h"

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
	return rec;
}

inline bar from_bar_record(const bar_record& r)
{
	return bar{r.date, r.symbol, r.open, r.high, r.low, r.close, r.volume};
}

inline tick from_tick_record(const tick_record& r)
{
	return tick{r.timestamp, r.symbol, r.price, r.quantity,
	            static_cast<uint8_t>(r.side)};
}

} // namespace provider
