#pragma once

// DataWrapper — single programmatic façade for multi-format market load
// (docs/internal/data-pipeline.md §5.5). Composition root; not a second buffer.

#include "data/market_source.h"
#include "data/market_sink.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class MarketSeries;

struct DataLoadOptions
{
	std::optional<std::chrono::system_clock::time_point> from;
	std::optional<std::chrono::system_clock::time_point> to;
	std::vector<std::string> symbols; // empty = all
	bool sort_after_load = true;
	bool fail_if_empty = true;
	// Multi-file: default fail-closed on any part failure (DR-REPLAY-03).
	// When true, keep rows accepted so far and return success if any accepted.
	bool allow_partial_sources = false;
	bool retain_streamed = false; // live default: do not grow series
	std::size_t reserve_hint = 0;
};

class DataWrapper
{
public:
	static DataWrapper from_uri(std::string_view uri, DataLoadOptions opt = {});
	static DataWrapper from_paths(const std::vector<std::filesystem::path>& paths,
	                              DataLoadOptions opt = {});
	static DataWrapper from_path(const std::filesystem::path& path,
	                             DataLoadOptions opt = {});
	static DataWrapper from_source(std::unique_ptr<IMarketSource> source,
	                               DataLoadOptions opt = {});

	// Batch materialize into series
	bool load(MarketSeries& out);

	// Stream into arbitrary sink
	StreamResult stream(IMarketSink& sink, std::atomic<bool>* halt = nullptr);

	IMarketSource& source();
	const DataLoadOptions& options() const { return opt_; }

private:
	DataWrapper(std::unique_ptr<IMarketSource> source, DataLoadOptions opt);

	std::unique_ptr<IMarketSource> source_;
	DataLoadOptions opt_;
};
