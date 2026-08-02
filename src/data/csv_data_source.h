#pragma once

#include "data/data_source.h"

#include <filesystem>

// CSV OHLCV bar source. Implements IMarketSource via IDataSource shim.
// Format knowledge lives here — not on MarketSeries (docs/internal/data-pipeline.md D-03/D-04).
class CsvDataSource : public IDataSource
{
	std::filesystem::path path_;

public:
	explicit CsvDataSource(std::filesystem::path path);
	bool load_data(std::shared_ptr<data_handler> handler) override;
	bool load_into(IMarketSink& sink, LoadStats* stats = nullptr) override;
};
