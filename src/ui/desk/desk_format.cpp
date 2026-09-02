#include "ui/desk/desk_format.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace truetest::ui::desk {
namespace {

bool usable(const std::optional<double>& value)
{
    return value && std::isfinite(*value);
}

std::string decimal(double value, int precision, bool signed_value = false)
{
    std::ostringstream out;
    if (signed_value && value > 0.0) out << '+';
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

int price_precision(double value)
{
    const double magnitude = std::abs(value);
    if (magnitude >= 1'000.0) return 2;
    if (magnitude >= 1.0) return 4;
    if (magnitude >= 0.01) return 6;
    return 8;
}

}  // namespace

std::string format_price(std::optional<double> value)
{
    return usable(value) ? decimal(*value, price_precision(*value)) : "—";
}

std::string format_money(std::optional<double> value, bool signed_value)
{
    return usable(value) ? "$" + decimal(*value, 2, signed_value) : "—";
}

std::string format_quantity(std::optional<double> value)
{
    if (!usable(value)) return "—";
    const double magnitude = std::abs(*value);
    return decimal(*value, magnitude >= 1'000.0 ? 2 : (magnitude >= 1.0 ? 4 : 6));
}

std::string format_bps(std::optional<double> value, bool signed_value)
{
    return usable(value) ? decimal(*value, 1, signed_value) + " bps" : "—";
}

std::string format_percent(std::optional<double> value, bool signed_value)
{
    return usable(value) ? decimal(*value, 2, signed_value) + "%" : "—";
}

std::string format_duration(std::optional<std::int64_t> seconds)
{
    if (!seconds || *seconds < 0) return "—";
    const std::int64_t value = *seconds;
    if (value < 60) return std::to_string(value) + "s";
    if (value < 3'600) return std::to_string(value / 60) + "m";
    if (value < 86'400) return std::to_string(value / 3'600) + "h";
    return std::to_string(value / 86'400) + "d";
}

std::string format_count(std::optional<std::size_t> value)
{
    return value ? std::to_string(*value) : "—";
}

}  // namespace truetest::ui::desk
