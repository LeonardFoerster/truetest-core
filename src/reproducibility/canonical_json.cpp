#include "canonical_json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace truetest::reproducibility {

namespace {

[[noreturn]] void type_error(std::string_view expected)
{
    throw std::invalid_argument("JSON value is not " + std::string(expected));
}

[[nodiscard]] bool valid_utf8(std::string_view input) noexcept
{
    std::size_t i = 0;
    while (i < input.size())
    {
        const auto first = static_cast<unsigned char>(input[i]);
        if (first < 0x80U)
        {
            ++i;
            continue;
        }

        std::size_t count = 0;
        std::uint32_t code_point = 0;
        if ((first & 0xe0U) == 0xc0U)
        {
            count = 2;
            code_point = first & 0x1fU;
            if (code_point < 2U)
                return false;
        }
        else if ((first & 0xf0U) == 0xe0U)
        {
            count = 3;
            code_point = first & 0x0fU;
        }
        else if ((first & 0xf8U) == 0xf0U)
        {
            count = 4;
            code_point = first & 0x07U;
        }
        else
        {
            return false;
        }
        if (i + count > input.size())
            return false;
        for (std::size_t j = 1; j < count; ++j)
        {
            const auto continuation = static_cast<unsigned char>(input[i + j]);
            if ((continuation & 0xc0U) != 0x80U)
                return false;
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if ((count == 3 && code_point < 0x800U)
            || (count == 4 && code_point < 0x10000U)
            || code_point > 0x10ffffU
            || (code_point >= 0xd800U && code_point <= 0xdfffU))
            return false;
        i += count;
    }
    return true;
}

void append_escaped_string(std::string& output, std::string_view value)
{
    if (!valid_utf8(value))
        throw std::invalid_argument("canonical JSON requires valid UTF-8");
    constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char byte : value)
    {
        switch (byte)
        {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (byte < 0x20U)
            {
                output += "\\u00";
                output.push_back(hex[byte >> 4U]);
                output.push_back(hex[byte & 0x0fU]);
            }
            else
            {
                output.push_back(static_cast<char>(byte));
            }
        }
    }
    output.push_back('"');
}

template <typename Integer>
void append_integer(std::string& output, Integer value)
{
    char buffer[32];
    const auto [end, error] = std::to_chars(std::begin(buffer), std::end(buffer), value);
    if (error != std::errc{})
        throw std::runtime_error("failed to serialize JSON integer");
    output.append(buffer, end);
}

void append_double(std::string& output, double value)
{
    if (!std::isfinite(value))
        throw std::invalid_argument("canonical JSON rejects NaN and infinity");
    if (value == 0.0)
    {
        output.push_back('0');
        return;
    }

    char buffer[64];
    const auto [end, error] = std::to_chars(
        std::begin(buffer), std::end(buffer), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (error != std::errc{})
        throw std::runtime_error("failed to serialize JSON number");

    std::string token(buffer, end);
    const std::size_t exponent = token.find_first_of("eE");
    if (exponent != std::string::npos)
    {
        token[exponent] = 'e';
        std::size_t cursor = exponent + 1;
        bool negative = false;
        if (cursor < token.size() && (token[cursor] == '+' || token[cursor] == '-'))
        {
            negative = token[cursor] == '-';
            ++cursor;
        }
        while (cursor + 1 < token.size() && token[cursor] == '0')
            ++cursor;
        const std::string digits = token.substr(cursor);
        token.resize(exponent + 1);
        if (negative)
            token.push_back('-');
        token += digits;
    }
    output += token;
}

void append_value(std::string& output, const CanonicalJsonValue& value)
{
    std::visit([&output](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>)
        {
            output += "null";
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            output += item ? "true" : "false";
        }
        else if constexpr (std::is_same_v<T, std::int64_t>
                           || std::is_same_v<T, std::uint64_t>)
        {
            append_integer(output, item);
        }
        else if constexpr (std::is_same_v<T, double>)
        {
            append_double(output, item);
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            append_escaped_string(output, item);
        }
        else if constexpr (std::is_same_v<T, CanonicalJsonValue::Array>)
        {
            output.push_back('[');
            for (std::size_t i = 0; i < item.size(); ++i)
            {
                if (i != 0)
                    output.push_back(',');
                append_value(output, item[i]);
            }
            output.push_back(']');
        }
        else
        {
            output.push_back('{');
            bool first = true;
            for (const auto& [key, child] : item)
            {
                if (!first)
                    output.push_back(',');
                first = false;
                append_escaped_string(output, key);
                output.push_back(':');
                append_value(output, child);
            }
            output.push_back('}');
        }
    }, value.storage());
}

class StrictParser final
{
public:
    explicit StrictParser(std::string_view input) : input_(input) {}

    [[nodiscard]] CanonicalJsonValue parse()
    {
        skip_whitespace();
        CanonicalJsonValue value = parse_value(0);
        skip_whitespace();
        if (position_ != input_.size())
            fail("trailing bytes");
        return value;
    }

private:
    [[noreturn]] void fail(std::string_view message) const
    {
        throw std::invalid_argument("invalid JSON at byte "
            + std::to_string(position_) + ": " + std::string(message));
    }

    void skip_whitespace() noexcept
    {
        while (position_ < input_.size())
        {
            const char c = input_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                break;
            ++position_;
        }
    }

    [[nodiscard]] bool consume(char expected) noexcept
    {
        if (position_ < input_.size() && input_[position_] == expected)
        {
            ++position_;
            return true;
        }
        return false;
    }

    void require_literal(std::string_view literal)
    {
        if (input_.substr(position_, literal.size()) != literal)
            fail("expected " + std::string(literal));
        position_ += literal.size();
    }

    [[nodiscard]] CanonicalJsonValue parse_value(std::size_t depth)
    {
        if (depth > 128)
            fail("nesting exceeds 128 levels");
        if (position_ >= input_.size())
            fail("unexpected end of input");
        switch (input_[position_])
        {
        case 'n': require_literal("null"); return nullptr;
        case 't': require_literal("true"); return true;
        case 'f': require_literal("false"); return false;
        case '"': return parse_string();
        case '[': return parse_array(depth + 1);
        case '{': return parse_object(depth + 1);
        default: return parse_number();
        }
    }

    [[nodiscard]] CanonicalJsonValue parse_array(std::size_t depth)
    {
        ++position_;
        CanonicalJsonValue::Array result;
        skip_whitespace();
        if (consume(']'))
            return result;
        for (;;)
        {
            skip_whitespace();
            result.push_back(parse_value(depth));
            skip_whitespace();
            if (consume(']'))
                return result;
            if (!consume(','))
                fail("expected ',' or ']'");
        }
    }

    [[nodiscard]] CanonicalJsonValue parse_object(std::size_t depth)
    {
        ++position_;
        CanonicalJsonValue::Object result;
        skip_whitespace();
        if (consume('}'))
            return result;
        for (;;)
        {
            skip_whitespace();
            if (position_ >= input_.size() || input_[position_] != '"')
                fail("expected object key");
            std::string key = parse_string().as_string();
            skip_whitespace();
            if (!consume(':'))
                fail("expected ':'");
            skip_whitespace();
            auto [iterator, inserted] = result.emplace(
                std::move(key), parse_value(depth));
            (void)iterator;
            if (!inserted)
                fail("duplicate object key");
            skip_whitespace();
            if (consume('}'))
                return result;
            if (!consume(','))
                fail("expected ',' or '}'");
        }
    }

    [[nodiscard]] static unsigned hex_digit(char value)
    {
        if (value >= '0' && value <= '9')
            return static_cast<unsigned>(value - '0');
        if (value >= 'a' && value <= 'f')
            return static_cast<unsigned>(value - 'a' + 10);
        if (value >= 'A' && value <= 'F')
            return static_cast<unsigned>(value - 'A' + 10);
        return 16U;
    }

    [[nodiscard]] std::uint32_t parse_hex_quad()
    {
        if (position_ + 4 > input_.size())
            fail("truncated Unicode escape");
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i)
        {
            const unsigned digit = hex_digit(input_[position_++]);
            if (digit == 16U)
                fail("invalid Unicode escape");
            value = (value << 4U) | digit;
        }
        return value;
    }

    void append_code_point(std::string& output, std::uint32_t value)
    {
        if (value <= 0x7fU)
        {
            output.push_back(static_cast<char>(value));
        }
        else if (value <= 0x7ffU)
        {
            output.push_back(static_cast<char>(0xc0U | (value >> 6U)));
            output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
        }
        else if (value <= 0xffffU)
        {
            output.push_back(static_cast<char>(0xe0U | (value >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
        }
        else
        {
            output.push_back(static_cast<char>(0xf0U | (value >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
        }
    }

    [[nodiscard]] CanonicalJsonValue parse_string()
    {
        ++position_;
        std::string output;
        while (position_ < input_.size())
        {
            const char c = input_[position_++];
            if (c == '"')
            {
                if (!valid_utf8(output))
                    fail("string is not valid UTF-8");
                return output;
            }
            if (static_cast<unsigned char>(c) < 0x20U)
                fail("unescaped control byte in string");
            if (c != '\\')
            {
                output.push_back(c);
                continue;
            }
            if (position_ >= input_.size())
                fail("truncated escape");
            const char escape = input_[position_++];
            switch (escape)
            {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t code_point = parse_hex_quad();
                if (code_point >= 0xd800U && code_point <= 0xdbffU)
                {
                    if (position_ + 2 > input_.size()
                        || input_[position_] != '\\'
                        || input_[position_ + 1] != 'u')
                        fail("high surrogate lacks low surrogate");
                    position_ += 2;
                    const std::uint32_t low = parse_hex_quad();
                    if (low < 0xdc00U || low > 0xdfffU)
                        fail("invalid low surrogate");
                    code_point = 0x10000U
                        + ((code_point - 0xd800U) << 10U)
                        + (low - 0xdc00U);
                }
                else if (code_point >= 0xdc00U && code_point <= 0xdfffU)
                {
                    fail("unpaired low surrogate");
                }
                append_code_point(output, code_point);
                break;
            }
            default: fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    [[nodiscard]] CanonicalJsonValue parse_number()
    {
        const std::size_t begin = position_;
        const bool negative = consume('-');
        if (position_ >= input_.size())
            fail("truncated number");
        if (consume('0'))
        {
            if (position_ < input_.size()
                && input_[position_] >= '0' && input_[position_] <= '9')
                fail("leading zero in number");
        }
        else
        {
            if (input_[position_] < '1' || input_[position_] > '9')
                fail("expected JSON value");
            while (position_ < input_.size()
                   && input_[position_] >= '0' && input_[position_] <= '9')
                ++position_;
        }

        bool floating = false;
        if (consume('.'))
        {
            floating = true;
            const std::size_t fraction_begin = position_;
            while (position_ < input_.size()
                   && input_[position_] >= '0' && input_[position_] <= '9')
                ++position_;
            if (position_ == fraction_begin)
                fail("fraction requires digits");
        }
        if (position_ < input_.size()
            && (input_[position_] == 'e' || input_[position_] == 'E'))
        {
            floating = true;
            ++position_;
            if (position_ < input_.size()
                && (input_[position_] == '+' || input_[position_] == '-'))
                ++position_;
            const std::size_t exponent_begin = position_;
            while (position_ < input_.size()
                   && input_[position_] >= '0' && input_[position_] <= '9')
                ++position_;
            if (position_ == exponent_begin)
                fail("exponent requires digits");
        }

        const std::string_view token = input_.substr(begin, position_ - begin);
        if (floating)
        {
            double value = 0.0;
            const auto [end, error] = std::from_chars(
                token.data(), token.data() + token.size(), value,
                std::chars_format::general);
            if (error != std::errc{} || end != token.data() + token.size()
                || !std::isfinite(value))
                fail("number is outside finite binary64 range");
            return value;
        }
        if (negative)
        {
            std::int64_t value = 0;
            const auto [end, error] = std::from_chars(
                token.data(), token.data() + token.size(), value);
            if (error != std::errc{} || end != token.data() + token.size())
                fail("signed integer is outside int64 range");
            return value;
        }
        std::uint64_t value = 0;
        const auto [end, error] = std::from_chars(
            token.data(), token.data() + token.size(), value);
        if (error != std::errc{} || end != token.data() + token.size())
            fail("unsigned integer is outside uint64 range");
        return value;
    }

    std::string_view input_;
    std::size_t position_{0};
};

} // namespace

CanonicalJsonValue CanonicalJsonValue::array(
    std::initializer_list<CanonicalJsonValue> values)
{
    return Array(values);
}

CanonicalJsonValue CanonicalJsonValue::object(
    std::initializer_list<Object::value_type> values)
{
    return Object(values);
}

bool CanonicalJsonValue::is_null() const noexcept
{
    return std::holds_alternative<std::nullptr_t>(storage_);
}

bool CanonicalJsonValue::is_bool() const noexcept
{
    return std::holds_alternative<bool>(storage_);
}

bool CanonicalJsonValue::is_integer() const noexcept
{
    return std::holds_alternative<std::int64_t>(storage_)
        || std::holds_alternative<std::uint64_t>(storage_);
}

bool CanonicalJsonValue::is_number() const noexcept
{
    return is_integer() || std::holds_alternative<double>(storage_);
}

bool CanonicalJsonValue::is_string() const noexcept
{
    return std::holds_alternative<std::string>(storage_);
}

bool CanonicalJsonValue::is_array() const noexcept
{
    return std::holds_alternative<Array>(storage_);
}

bool CanonicalJsonValue::is_object() const noexcept
{
    return std::holds_alternative<Object>(storage_);
}

bool CanonicalJsonValue::as_bool() const
{
    if (const auto* value = std::get_if<bool>(&storage_))
        return *value;
    type_error("a boolean");
}

std::int64_t CanonicalJsonValue::as_i64() const
{
    if (const auto* value = std::get_if<std::int64_t>(&storage_))
        return *value;
    if (const auto* value = std::get_if<std::uint64_t>(&storage_))
    {
        if (*value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return static_cast<std::int64_t>(*value);
    }
    type_error("an int64-compatible integer");
}

std::uint64_t CanonicalJsonValue::as_u64() const
{
    if (const auto* value = std::get_if<std::uint64_t>(&storage_))
        return *value;
    if (const auto* value = std::get_if<std::int64_t>(&storage_); value && *value >= 0)
        return static_cast<std::uint64_t>(*value);
    type_error("a uint64-compatible integer");
}

double CanonicalJsonValue::as_double() const
{
    if (const auto* value = std::get_if<double>(&storage_))
        return *value;
    if (const auto* value = std::get_if<std::int64_t>(&storage_))
        return static_cast<double>(*value);
    if (const auto* value = std::get_if<std::uint64_t>(&storage_))
        return static_cast<double>(*value);
    type_error("a number");
}

const std::string& CanonicalJsonValue::as_string() const
{
    if (const auto* value = std::get_if<std::string>(&storage_))
        return *value;
    type_error("a string");
}

const CanonicalJsonValue::Array& CanonicalJsonValue::as_array() const
{
    if (const auto* value = std::get_if<Array>(&storage_))
        return *value;
    type_error("an array");
}

CanonicalJsonValue::Array& CanonicalJsonValue::as_array()
{
    if (auto* value = std::get_if<Array>(&storage_))
        return *value;
    type_error("an array");
}

const CanonicalJsonValue::Object& CanonicalJsonValue::as_object() const
{
    if (const auto* value = std::get_if<Object>(&storage_))
        return *value;
    type_error("an object");
}

CanonicalJsonValue::Object& CanonicalJsonValue::as_object()
{
    if (auto* value = std::get_if<Object>(&storage_))
        return *value;
    type_error("an object");
}

bool CanonicalJsonValue::contains(std::string_view key) const
{
    if (const auto* object = std::get_if<Object>(&storage_))
        return object->find(key) != object->end();
    return false;
}

const CanonicalJsonValue& CanonicalJsonValue::at(std::string_view key) const
{
    const auto& object = as_object();
    const auto iterator = object.find(key);
    if (iterator == object.end())
        throw std::out_of_range("missing JSON field: " + std::string(key));
    return iterator->second;
}

CanonicalJsonValue& CanonicalJsonValue::operator[](std::string key)
{
    return as_object()[std::move(key)];
}

std::string serialize_canonical_json(const CanonicalJsonValue& value)
{
    std::string output;
    append_value(output, value);
    return output;
}

CanonicalJsonValue parse_json_strict(std::string_view text)
{
    if (!valid_utf8(text))
        throw std::invalid_argument("JSON input is not valid UTF-8");
    return StrictParser(text).parse();
}

} // namespace truetest::reproducibility
