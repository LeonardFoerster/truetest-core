#pragma once

#include <stdexcept>
#include <string>

class pool_exhausted : public std::runtime_error
{
public:
    explicit pool_exhausted(const std::string& pool_name)
        : std::runtime_error("object pool exhausted: " + pool_name)
    {}
};