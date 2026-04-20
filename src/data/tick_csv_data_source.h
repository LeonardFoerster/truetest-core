#pragma once

#include "data_source.h"
#include <string>

class TickCsvDataSource : public IDataSource
{
public:
    explicit TickCsvDataSource(const std::string& path) : path_(path) {}
    bool load_data(std::shared_ptr<data_handler> handler) override;

private:
    std::string path_;
};
