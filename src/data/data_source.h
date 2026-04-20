#pragma once
#include <memory>

class data_handler;

class IDataSource {
public:
    virtual ~IDataSource() = default;
    virtual bool load_data(std::shared_ptr<data_handler> handler) = 0;
};
