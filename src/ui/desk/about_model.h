#pragma once

#include <string>
#include <string_view>

namespace truetest::ui::desk {

struct DeskBuildInfo
{
    std::string_view version;
    std::string_view git_sha;
    std::string_view git_state_at_configure;
    std::string_view configured_at_utc;
    std::string_view build_type;
    std::string_view compiler;
    std::string_view target;
    bool live_orders_compiled = false;
};

constexpr std::string_view build_value_or_unknown(std::string_view value) noexcept
{
    return value.empty() ? std::string_view{"unknown"} : value;
}

inline std::string format_build_identity(const DeskBuildInfo& info)
{
    std::string result;
    result.reserve(192);
    auto append = [&](std::string_view label, std::string_view value) {
        if (!result.empty())
            result.push_back('\n');
        result.append(label);
        result.append(": ");
        result.append(build_value_or_unknown(value));
    };
    append("version", info.version);
    append("git sha at configure", info.git_sha);
    append("worktree at configure", info.git_state_at_configure);
    append("configured at UTC", info.configured_at_utc);
    append("build type", info.build_type);
    append("compiler", info.compiler);
    append("target", info.target);
    append("live orders compiled", info.live_orders_compiled ? "yes" : "no");
    return result;
}

} // namespace truetest::ui::desk
