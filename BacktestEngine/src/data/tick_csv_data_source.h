#pragma once

#include "data_source.h"
#include <string>

// Loads tick-level data from CSV files.
// Expected CSV format: timestamp_ms,symbol,price,quantity,side
// Where side is: B (bid/buy aggressor), A (ask/sell aggressor), or empty/U (unknown)
class TickCsvDataSource : public IDataSource
{
public:
    explicit TickCsvDataSource(const std::string& path) : path_(path) {}
    bool load_data(std::shared_ptr<data_handler> handler) override;

private:
    std::string path_;
};
