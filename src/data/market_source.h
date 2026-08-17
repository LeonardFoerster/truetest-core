#pragma once

// IMarketSource — format adapters emit domain records into a sink (docs/internal/data-pipeline.md §5.3).
// Hard rule: prefer not including market_series.h — only IMarketSink + market_types.

#include "data/market_sink.h"

#include <atomic>
#include <cstddef>

enum class stream_termination
{
	clean_eof,
	operator_stop,
	engine_halt,
	unsupported,
	transport_open_failure,
	header_failure,
	transport_failure,
	parse_or_sink_failure,
	runtime_failure
};

struct StreamResult
{
	stream_termination termination = stream_termination::transport_failure;
	std::size_t accepted = 0;
	std::size_t rejected = 0;

	bool success() const
	{
		return termination == stream_termination::clean_eof
		    || termination == stream_termination::operator_stop;
	}
};

class IMarketSource
{
public:
	virtual ~IMarketSource() = default;

	// Batch: push all records into sink in source order; return false on hard
	// failure (I/O, schema). Ordering/final sorting belongs to the composing
	// batch façade so a failed multi-source load can remain transactional.
	// Sources used through DataWrapper must treat the sink as append-only: they
	// must not clear, reorder, or otherwise mutate records that predate this
	// call. That lets the façade fail atomically with an O(1) checkpoint.
	virtual bool load_into(IMarketSink& sink, LoadStats* stats = nullptr) = 0;

	// Stream: optional; default false = not supported
	virtual bool supports_stream() const { return false; }

	virtual StreamResult stream_into(IMarketSink& sink,
	                                 std::atomic<bool>* halt = nullptr,
	                                 LoadStats* stats = nullptr)
	{
		(void)sink;
		(void)halt;
		(void)stats;
		return {stream_termination::unsupported};
	}
};
