#pragma once

#include "data/data_source.h"

#include <string>

class TickCsvDataSource : public IDataSource
{
	std::string path_;

public:
	explicit TickCsvDataSource(const std::string& path) : path_(path) {}
	bool load_data(std::shared_ptr<data_handler> handler) override;
	bool load_into(IMarketSink& sink, LoadStats* stats = nullptr) override;
};
