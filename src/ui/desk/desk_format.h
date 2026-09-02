#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace truetest::ui::desk {

std::string format_price(std::optional<double> value);
std::string format_money(std::optional<double> value, bool signed_value = false);
std::string format_quantity(std::optional<double> value);
std::string format_bps(std::optional<double> value, bool signed_value = false);
std::string format_percent(std::optional<double> value, bool signed_value = false);
std::string format_duration(std::optional<std::int64_t> seconds);
std::string format_count(std::optional<std::size_t> value);

}  // namespace truetest::ui::desk
