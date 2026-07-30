#include "maintenance_margin_table.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

namespace truetest::risk {
namespace {

std::size_t skip_ws(const std::string& s, std::size_t pos)
{
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
        ++pos;
    return pos;
}

std::optional<double> extract_number(const std::string& object, const char* key)
{
    const std::string quoted_key = std::string("\"") + key + "\"";
    std::size_t pos = object.find(quoted_key);
    if (pos == std::string::npos)
        return std::nullopt;

    pos = object.find(':', pos + quoted_key.size());
    if (pos == std::string::npos)
        return std::nullopt;

    pos = skip_ws(object, pos + 1);
    if (pos >= object.size())
        return std::nullopt;

    if (object[pos] == '"') {
        std::size_t end = object.find('"', pos + 1);
        if (end == std::string::npos)
            return std::nullopt;
        std::string value = object.substr(pos + 1, end - pos - 1);
        char* parse_end = nullptr;
        const double parsed = std::strtod(value.c_str(), &parse_end);
        if (parse_end == value.c_str())
            return std::nullopt;
        return parsed;
    }

    char* parse_end = nullptr;
    const double parsed = std::strtod(object.c_str() + pos, &parse_end);
    if (parse_end == object.c_str() + pos)
        return std::nullopt;
    return parsed;
}

std::optional<std::string> first_brackets_array(const std::string& body)
{
    const std::string key = "\"brackets\"";
    std::size_t pos = body.find(key);
    if (pos == std::string::npos)
        return std::nullopt;

    pos = body.find(':', pos + key.size());
    if (pos == std::string::npos)
        return std::nullopt;

    pos = body.find('[', pos + 1);
    if (pos == std::string::npos)
        return std::nullopt;

    const std::size_t start = pos;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (; pos < body.size(); ++pos) {
        const char c = body[pos];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
        } else if (c == '[') {
            ++depth;
        } else if (c == ']') {
            --depth;
            if (depth == 0)
                return body.substr(start + 1, pos - start - 1);
        }
    }

    return std::nullopt;
}

void parse_bracket_objects(const std::string& brackets, std::vector<MarginTier>& tiers)
{
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    std::size_t object_start = std::string::npos;

    for (std::size_t pos = 0; pos < brackets.size(); ++pos) {
        const char c = brackets[pos];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            if (depth == 0)
                object_start = pos;
            ++depth;
        } else if (c == '}') {
            if (depth <= 0)
                continue;
            --depth;
            if (depth == 0 && object_start != std::string::npos) {
                std::string object = brackets.substr(object_start, pos - object_start + 1);
                MarginTier tier;
                tier.notional_cap = extract_number(object, "notionalCap")
                                        .value_or(std::numeric_limits<double>::max());
                tier.maintenance_margin_rate = extract_number(object, "maintMarginRatio")
                                                   .value_or(0.0);
                tier.maint_amount = extract_number(object, "cum").value_or(0.0);
                tiers.push_back(tier);
                object_start = std::string::npos;
            }
        }
    }
}

} // namespace

void MaintenanceMarginTable::load_from_leverage_bracket_json(const std::string& body)
{
    tiers_.clear();

    auto brackets = first_brackets_array(body);
    if (!brackets)
        return;

    parse_bracket_objects(*brackets, tiers_);

    std::sort(tiers_.begin(), tiers_.end(),
              [](const MarginTier& a, const MarginTier& b) {
                  return a.notional_cap < b.notional_cap;
              });
}

double MaintenanceMarginTable::maintenance_margin_rate_for_notional(double notional) const
{
    if (tiers_.empty()) {
        return 0.005; // safe fallback (original default)
    }

    for (const auto& t : tiers_) {
        if (notional <= t.notional_cap) {
            return t.maintenance_margin_rate;
        }
    }
    // Beyond last tier - use the highest tier's rate
    return tiers_.back().maintenance_margin_rate;
}

double MaintenanceMarginTable::maint_amount_for_notional(double notional) const
{
    if (tiers_.empty()) {
        return 0.0;
    }

    for (const auto& t : tiers_) {
        if (notional <= t.notional_cap) {
            return t.maint_amount;
        }
    }
    return tiers_.back().maint_amount;
}

} // namespace truetest::risk
