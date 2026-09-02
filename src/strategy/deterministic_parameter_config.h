#pragma once

#include "strategy_interface.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace truetest::strategy_config {

// Deterministic manifests treat param_def::default_value as the strategy's
// current effective value. Validate that contract every time the cold-path
// configuration is applied so a setter cannot silently clamp or truncate a
// value that is hashed under a different spelling.
[[nodiscard]] inline std::vector<param_def> checked_parameter_schema(
    const IStrategy& strategy)
{
    auto schema = strategy.get_param_schema();
    std::map<std::string, bool, std::less<>> names;
    for (const auto& parameter : schema)
    {
        if (parameter.name.empty()
            || !std::isfinite(parameter.default_value)
            || !std::isfinite(parameter.min_value)
            || !std::isfinite(parameter.max_value)
            || parameter.min_value > parameter.max_value
            || parameter.default_value < parameter.min_value
            || parameter.default_value > parameter.max_value
            || !names.emplace(parameter.name, true).second)
            throw std::invalid_argument(
                "strategy exposes an invalid deterministic parameter schema");
    }
    return schema;
}

[[nodiscard]] inline bool set_parameter_if_declared(
    IStrategy& strategy, std::string_view name, double value,
    bool required = false)
{
    const auto before = checked_parameter_schema(strategy);
    const auto parameter = std::find_if(
        before.begin(), before.end(), [&](const param_def& candidate) {
            return candidate.name == name;
        });
    if (parameter == before.end())
    {
        if (required)
            throw std::invalid_argument(
                "strategy parameter is absent from schema: "
                + std::string(name));
        return false;
    }
    if (!std::isfinite(value) || value < parameter->min_value
        || value > parameter->max_value)
        throw std::invalid_argument(
            "strategy parameter is outside its deterministic schema range: "
            + std::string(name));

    strategy.set_param(std::string(name), value);
    const auto after = checked_parameter_schema(strategy);
    const auto applied = std::find_if(
        after.begin(), after.end(), [&](const param_def& candidate) {
            return candidate.name == name;
        });
    if (applied == after.end() || applied->default_value != value)
        throw std::invalid_argument(
            "strategy parameter cannot represent the requested deterministic value exactly: "
            + std::string(name));
    return true;
}

[[nodiscard]] inline std::map<std::string, double, std::less<>>
effective_parameter_values(const IStrategy& strategy)
{
    std::map<std::string, double, std::less<>> result;
    for (const auto& parameter : checked_parameter_schema(strategy))
        result.emplace(parameter.name, parameter.default_value);
    return result;
}

} // namespace truetest::strategy_config
