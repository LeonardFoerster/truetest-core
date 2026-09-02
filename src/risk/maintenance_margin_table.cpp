#include "maintenance_margin_table.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace truetest::risk {
namespace {

struct bracket_payload
{
    std::string_view symbol;
    std::vector<MarginTier> tiers;
};

class strict_json_reader
{
public:
    explicit strict_json_reader(std::string_view input) : input_(input) {}

    std::optional<bracket_payload> parse_bracket_response()
    {
        skip_ws();
        bracket_payload payload;
        bool ok = false;
        if (peek() == '{') {
            ok = parse_response_object(payload, 0);
        } else if (peek() == '[') {
            ++pos_;
            skip_ws();
            ok = parse_response_object(payload, 1);
            skip_ws();
            if (!consume(']')) ok = false; // exactly one response object
        }
        skip_ws();
        if (!ok || pos_ != input_.size() || payload.symbol.empty()
            || payload.tiers.empty())
            return std::nullopt;
        return payload;
    }

private:
    void skip_ws()
    {
        while (pos_ < input_.size()
               && (input_[pos_] == ' ' || input_[pos_] == '\t'
                   || input_[pos_] == '\n' || input_[pos_] == '\r'))
            ++pos_;
    }

    char peek() const noexcept
    {
        return pos_ < input_.size() ? input_[pos_] : '\0';
    }

    bool consume(char expected)
    {
        skip_ws();
        if (peek() != expected) return false;
        ++pos_;
        return true;
    }

    bool parse_string(std::string_view* plain = nullptr)
    {
        skip_ws();
        if (peek() != '"') return false;
        const std::size_t begin = ++pos_;
        bool escaped = false;
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_]);
            if (c < 0x20) return false;
            if (c == '"') {
                if (plain) {
                    if (escaped) return false;
                    *plain = input_.substr(begin, pos_ - begin);
                }
                ++pos_;
                return true;
            }
            if (c != '\\') {
                ++pos_;
                continue;
            }
            escaped = true;
            if (++pos_ >= input_.size()) return false;
            const char esc = input_[pos_++];
            if (esc == 'u') {
                for (int i = 0; i < 4; ++i) {
                    if (pos_ >= input_.size()
                        || !std::isxdigit(
                            static_cast<unsigned char>(input_[pos_++])))
                        return false;
                }
            } else if (std::string_view{"\"\\/bfnrt"}.find(esc)
                       == std::string_view::npos) {
                return false;
            }
        }
        return false;
    }

    bool parse_number()
    {
        skip_ws();
        const std::size_t begin = pos_;
        if (peek() == '-') ++pos_;
        if (peek() == '0') {
            ++pos_;
            if (std::isdigit(static_cast<unsigned char>(peek()))) return false;
        } else {
            if (!std::isdigit(static_cast<unsigned char>(peek())) || peek() == '0')
                return false;
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        if (peek() == '.') {
            ++pos_;
            if (!std::isdigit(static_cast<unsigned char>(peek()))) return false;
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!std::isdigit(static_cast<unsigned char>(peek()))) return false;
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        return pos_ > begin;
    }

    bool consume_literal(std::string_view literal)
    {
        skip_ws();
        if (input_.substr(pos_, literal.size()) != literal) return false;
        pos_ += literal.size();
        return true;
    }

    bool skip_value(unsigned depth)
    {
        if (depth > 64) return false;
        skip_ws();
        if (peek() == '"') return parse_string();
        if (peek() == '{') return skip_object(depth + 1);
        if (peek() == '[') return skip_array(depth + 1);
        if (peek() == 't') return consume_literal("true");
        if (peek() == 'f') return consume_literal("false");
        if (peek() == 'n') return consume_literal("null");
        return parse_number();
    }

    bool skip_object(unsigned depth)
    {
        if (!consume('{')) return false;
        skip_ws();
        if (consume('}')) return true;
        while (true) {
            if (!parse_string() || !consume(':') || !skip_value(depth))
                return false;
            skip_ws();
            if (consume('}')) return true;
            if (!consume(',')) return false;
        }
    }

    bool skip_array(unsigned depth)
    {
        if (!consume('[')) return false;
        skip_ws();
        if (consume(']')) return true;
        while (true) {
            if (!skip_value(depth)) return false;
            skip_ws();
            if (consume(']')) return true;
            if (!consume(',')) return false;
        }
    }

    bool parse_financial_number(double& output)
    {
        skip_ws();
        std::string_view token;
        if (peek() == '"') {
            if (!parse_string(&token)) return false;
        } else {
            const std::size_t begin = pos_;
            if (!parse_number()) return false;
            token = input_.substr(begin, pos_ - begin);
        }
        const auto [end, error] = std::from_chars(
            token.data(), token.data() + token.size(), output,
            std::chars_format::general);
        return error == std::errc{} && end == token.data() + token.size()
            && std::isfinite(output);
    }

    bool parse_tier_object(MarginTier& tier, unsigned depth)
    {
        if (depth > 64 || !consume('{')) return false;
        bool have_floor = false;
        bool have_cap = false;
        bool have_rate = false;
        bool have_cum = false;
        skip_ws();
        if (peek() == '}') return false;
        while (true) {
            std::string_view key;
            if (!parse_string(&key) || !consume(':')) return false;
            if (key == "notionalFloor") {
                if (have_floor || !parse_financial_number(tier.notional_floor))
                    return false;
                have_floor = true;
            } else if (key == "notionalCap") {
                if (have_cap || !parse_financial_number(tier.notional_cap))
                    return false;
                have_cap = true;
            } else if (key == "maintMarginRatio") {
                if (have_rate
                    || !parse_financial_number(tier.maintenance_margin_rate))
                    return false;
                have_rate = true;
            } else if (key == "cum") {
                if (have_cum || !parse_financial_number(tier.maint_amount))
                    return false;
                have_cum = true;
            } else if (!skip_value(depth + 1)) {
                return false;
            }
            skip_ws();
            if (consume('}')) break;
            if (!consume(',')) return false;
        }
        return have_floor && have_cap && have_rate && have_cum
            && tier.notional_floor >= 0.0
            && tier.notional_cap > tier.notional_floor
            && tier.maintenance_margin_rate > 0.0
            && tier.maintenance_margin_rate < 1.0
            && tier.maint_amount >= 0.0
            && tier.maint_amount <= tier.notional_cap;
    }

    bool parse_tier_array(std::vector<MarginTier>& output, unsigned depth)
    {
        if (depth > 64 || !consume('[')) return false;
        skip_ws();
        if (peek() == ']') return false;
        while (true) {
            MarginTier tier{};
            if (!parse_tier_object(tier, depth + 1)) return false;
            output.push_back(tier);
            skip_ws();
            if (consume(']')) return true;
            if (!consume(',')) return false;
        }
    }

    bool parse_response_object(bracket_payload& payload, unsigned depth)
    {
        if (!consume('{')) return false;
        bool have_symbol = false;
        bool have_brackets = false;
        skip_ws();
        if (peek() == '}') return false;
        while (true) {
            std::string_view key;
            if (!parse_string(&key) || !consume(':')) return false;
            if (key == "symbol") {
                if (have_symbol || !parse_string(&payload.symbol)) return false;
                have_symbol = true;
            } else if (key == "brackets") {
                if (have_brackets
                    || !parse_tier_array(payload.tiers, depth + 1))
                    return false;
                have_brackets = true;
            } else if (!skip_value(depth + 1)) {
                return false;
            }
            skip_ws();
            if (consume('}')) break;
            if (!consume(',')) return false;
        }
        return have_symbol && have_brackets;
    }

    std::string_view input_;
    std::size_t pos_ = 0;
};

} // namespace

bool MaintenanceMarginTable::load_from_leverage_bracket_json(
    const std::string& body, std::string_view expected_symbol)
{
    tiers_.clear();
    valid_ = false;

    auto payload = strict_json_reader(body).parse_bracket_response();
    if (!payload || (!expected_symbol.empty()
                     && payload->symbol != expected_symbol))
        return false;

    std::vector<MarginTier> staged = std::move(payload->tiers);

    std::sort(staged.begin(), staged.end(),
              [](const MarginTier& a, const MarginTier& b) {
                  return a.notional_cap < b.notional_cap;
              });
    for (std::size_t i = 1; i < staged.size(); ++i) {
        if (!(staged[i].notional_cap > staged[i - 1].notional_cap)
            || staged[i].notional_floor != staged[i - 1].notional_cap
            || staged[i].maintenance_margin_rate
                < staged[i - 1].maintenance_margin_rate)
            return false;
    }
    if (staged.front().notional_floor != 0.0)
        return false;
    tiers_ = std::move(staged);
    valid_ = true;
    return true;
}

double MaintenanceMarginTable::maintenance_margin_rate_for_notional(double notional) const
{
    if (!valid_ || tiers_.empty())
        return 1.0; // conservative: 100% maintenance, never a permissive NaN

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
    if (!valid_ || tiers_.empty())
        return std::numeric_limits<double>::infinity();

    for (const auto& t : tiers_) {
        if (notional <= t.notional_cap) {
            return t.maint_amount;
        }
    }
    return tiers_.back().maint_amount;
}

} // namespace truetest::risk
