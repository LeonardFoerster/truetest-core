#pragma once
#include "data_source.h"
#include <string>
#include <memory>

class BinaryCacheSource : public IDataSource {
    std::unique_ptr<IDataSource> fallback_;
    std::string cache_path_;
public:
    BinaryCacheSource(std::unique_ptr<IDataSource> fallback, std::string cache_path);
    bool load_data(std::shared_ptr<data_handler> handler) override;
};
