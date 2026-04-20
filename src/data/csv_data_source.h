#pragma once
#include "data_source.h"
#include <string>
#include <filesystem>

class CsvDataSource : public IDataSource {
    std::filesystem::path path_;
public:
    explicit CsvDataSource(std::filesystem::path path);
    bool load_data(std::shared_ptr<data_handler> handler) override;
};
