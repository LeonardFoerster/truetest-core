#pragma once

#include "data/data_source.h"

#include <string>

class TickCsvDataSource : public IDataSource
{
	std::string path_;

public:
	explicit TickCsvDataSource(const std::string& path) : path_(path) {}
	// Low-level IMarketSource path emits source order. DataWrapper sorts a
	// successful complete batch; direct legacy callers use load_data(), which
	// retains the historical sorted-tape behavior.
	bool load_data(std::shared_ptr<data_handler> handler) override;
	bool load_into(IMarketSink& sink, LoadStats* stats = nullptr) override;
};
