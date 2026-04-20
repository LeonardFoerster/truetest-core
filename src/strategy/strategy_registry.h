#pragma once

#include "strategy_interface.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>

using strategy_factory = std::function<std::shared_ptr<IStrategy>()>;

class StrategyRegistry
{
public:
    static StrategyRegistry& instance()
    {
        static StrategyRegistry reg;
        return reg;
    }

    void register_strategy(const std::string& name, strategy_factory factory)
    {
        factories_[name] = std::move(factory);
    }

    std::shared_ptr<IStrategy> create(const std::string& name) const
    {
        auto it = factories_.find(name);
        if (it == factories_.end())
            throw std::runtime_error(
                "StrategyRegistry: unknown strategy '" + name + "'");
        return it->second();
    }

    std::vector<std::string> available() const
    {
        std::vector<std::string> names;
        names.reserve(factories_.size());
        for (const auto& [name, _] : factories_)
            names.push_back(name);
        return names;
    }

    bool has(const std::string& name) const
    {
        return factories_.count(name) > 0;
    }

private:
    StrategyRegistry() = default;
    std::unordered_map<std::string, strategy_factory> factories_;
};

#define REGISTER_STRATEGY(name, factory) \
    namespace { \
        static const bool _strat_reg_##__LINE__ = []() { \
            StrategyRegistry::instance().register_strategy(name, factory); \
            return true; \
        }(); \
    }
