#pragma once

// IMarketSource — format adapters emit domain records into a sink (docs/internal/data-pipeline.md §5.3).
// Hard rule: prefer not including market_series.h — only IMarketSink + market_types.

#include "data/market_sink.h"

#include <atomic>

class IMarketSource
{
public:
	virtual ~IMarketSource() = default;

	// Batch: push all records into sink; return false on hard failure (I/O, schema)
	virtual bool load_into(IMarketSink& sink, LoadStats* stats = nullptr) = 0;

	// Stream: optional; default false = not supported
	virtual bool supports_stream() const { return false; }

	virtual bool stream_into(IMarketSink& sink,
	                         std::atomic<bool>* halt = nullptr,
	                         LoadStats* stats = nullptr)
	{
		(void)sink;
		(void)halt;
		(void)stats;
		return false;
	}
};
