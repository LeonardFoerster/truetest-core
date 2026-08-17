#pragma once

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace provider_recovery
{

// Minimal allocation-free JSON recognizer for cold-path restart responses.
// Recovery must distinguish an authoritative empty array from malformed data;
// the venue adapters' fast field scanners intentionally do not provide that
// distinction themselves.
class payload_parser
{
public:
    enum class member_result { missing, unique, invalid_or_duplicate };
    explicit payload_parser(std::string_view input) : input_(input) {}

    bool valid_document()
    {
        pos_ = 0;
        skip_ws();
        if (!value(0)) return false;
        skip_ws();
        return pos_ == input_.size();
    }

    bool valid_object_array_document()
    {
        pos_ = 0;
        skip_ws();
        if (!consume('[')) return false;
        skip_ws();
        if (consume(']')) return at_document_end();
        for (;;)
        {
            if (!object(1)) return false;
            skip_ws();
            if (consume(']')) return at_document_end();
            if (!consume(',')) return false;
            skip_ws();
        }
    }

    bool valid_object_document()
    {
        pos_ = 0;
        skip_ws();
        if (!object(0)) return false;
        return at_document_end();
    }

    bool find_top_level_member(std::string_view wanted,
                               std::string_view& value_out)
    {
        return inspect_top_level_member(wanted, value_out)
            == member_result::unique;
    }

    member_result inspect_top_level_member(std::string_view wanted,
                                           std::string_view& value_out)
    {
        value_out = {};
        if (!valid_object_document()) return member_result::invalid_or_duplicate;

        pos_ = 0;
        skip_ws();
        if (!consume('{')) return member_result::invalid_or_duplicate;
        skip_ws();
        if (consume('}')) return member_result::missing;
        bool found = false;
        for (;;)
        {
            std::string_view key;
            if (!plain_string(key)) return member_result::invalid_or_duplicate;
            skip_ws();
            if (!consume(':')) return member_result::invalid_or_duplicate;
            skip_ws();
            const auto begin = pos_;
            if (!value(1)) return member_result::invalid_or_duplicate;
            const auto end = pos_;
            if (key == wanted)
            {
                if (found) return member_result::invalid_or_duplicate;
                value_out = input_.substr(begin, end - begin);
                found = true;
            }
            skip_ws();
            if (consume('}'))
                return found ? member_result::unique : member_result::missing;
            if (!consume(',')) return member_result::invalid_or_duplicate;
            skip_ws();
        }
    }

private:
    static constexpr unsigned max_depth = 64;

    bool at_document_end()
    {
        skip_ws();
        return pos_ == input_.size();
    }

    void skip_ws()
    {
        while (pos_ < input_.size())
        {
            const char c = input_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
    }

    bool consume(char expected)
    {
        if (pos_ >= input_.size() || input_[pos_] != expected) return false;
        ++pos_;
        return true;
    }

    bool literal(std::string_view text)
    {
        if (input_.substr(pos_, text.size()) != text) return false;
        pos_ += text.size();
        return true;
    }

    bool string()
    {
        if (!consume('"')) return false;
        while (pos_ < input_.size())
        {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') continue;
            if (pos_ >= input_.size()) return false;
            const char escaped = input_[pos_++];
            if (escaped == 'u')
            {
                for (int i = 0; i < 4; ++i)
                {
                    if (pos_ >= input_.size()
                        || !std::isxdigit(
                            static_cast<unsigned char>(input_[pos_++])))
                        return false;
                }
            }
            else if (std::string_view{"\"\\/bfnrt"}.find(escaped)
                     == std::string_view::npos)
                return false;
        }
        return false;
    }

    // Top-level venue envelope keys are ASCII. Reject escaped keys for lookup
    // rather than accepting an ambiguous spelling in a safety decision.
    bool plain_string(std::string_view& out)
    {
        if (!consume('"')) return false;
        const auto begin = pos_;
        while (pos_ < input_.size())
        {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c == '"')
            {
                out = input_.substr(begin, pos_ - begin - 1);
                return true;
            }
            if (c < 0x20 || c == '\\') return false;
        }
        return false;
    }

    bool number()
    {
        const auto begin = pos_;
        if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
        if (pos_ >= input_.size()) return false;
        if (input_[pos_] == '0')
            ++pos_;
        else
        {
            if (input_[pos_] < '1' || input_[pos_] > '9') return false;
            while (pos_ < input_.size()
                   && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        }
        if (pos_ < input_.size() && input_[pos_] == '.')
        {
            ++pos_;
            const auto fraction = pos_;
            while (pos_ < input_.size()
                   && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
            if (pos_ == fraction) return false;
        }
        if (pos_ < input_.size()
            && (input_[pos_] == 'e' || input_[pos_] == 'E'))
        {
            ++pos_;
            if (pos_ < input_.size()
                && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            const auto exponent = pos_;
            while (pos_ < input_.size()
                   && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
            if (pos_ == exponent) return false;
        }
        return pos_ != begin;
    }

    bool array(unsigned depth)
    {
        if (depth > max_depth || !consume('[')) return false;
        skip_ws();
        if (consume(']')) return true;
        for (;;)
        {
            if (!value(depth + 1)) return false;
            skip_ws();
            if (consume(']')) return true;
            if (!consume(',')) return false;
            skip_ws();
        }
    }

    bool object(unsigned depth)
    {
        if (depth > max_depth || !consume('{')) return false;
        skip_ws();
        if (consume('}')) return true;
        for (;;)
        {
            if (!string()) return false;
            skip_ws();
            if (!consume(':')) return false;
            skip_ws();
            if (!value(depth + 1)) return false;
            skip_ws();
            if (consume('}')) return true;
            if (!consume(',')) return false;
            skip_ws();
        }
    }

    bool value(unsigned depth)
    {
        if (depth > max_depth || pos_ >= input_.size()) return false;
        switch (input_[pos_])
        {
        case '{': return object(depth + 1);
        case '[': return array(depth + 1);
        case '"': return string();
        case 't': return literal("true");
        case 'f': return literal("false");
        case 'n': return literal("null");
        default: return number();
        }
    }

    std::string_view input_;
    std::size_t pos_ = 0;
};

inline bool is_valid_document(std::string_view payload)
{
    return payload_parser(payload).valid_document();
}

inline bool is_authoritative_object_array(std::string_view payload)
{
    return payload_parser(payload).valid_object_array_document();
}

inline bool is_authoritative_object(std::string_view payload)
{
    return payload_parser(payload).valid_object_document();
}

inline bool top_level_member(std::string_view payload,
                             std::string_view key,
                             std::string_view& value_out)
{
    return payload_parser(payload).find_top_level_member(key, value_out);
}

inline std::string_view trim_json_ws(std::string_view value)
{
    while (!value.empty() && std::isspace(
               static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(
               static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

// Safety decisions must never combine document validation with a recursive
// venue field scanner: a nested field is not an authoritative envelope field.
// These helpers both require a unique top-level member and validate its exact
// scalar representation.  Decision-critical venue strings are ASCII and may
// not use JSON escapes, keeping comparisons allocation-free and unambiguous.
inline bool top_level_plain_string(std::string_view payload,
                                   std::string_view key,
                                   std::string_view& value_out)
{
    std::string_view raw;
    if (!top_level_member(payload, key, raw)) return false;
    raw = trim_json_ws(raw);
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"')
        return false;
    raw.remove_prefix(1);
    raw.remove_suffix(1);
    if (raw.find('\\') != std::string_view::npos) return false;
    value_out = raw;
    return true;
}

inline bool top_level_exact_string(std::string_view payload,
                                   std::string_view key,
                                   std::string_view expected)
{
    std::string_view actual;
    return top_level_plain_string(payload, key, actual) && actual == expected;
}

inline bool top_level_scalar_text(std::string_view payload,
                                  std::string_view key,
                                  std::string_view& value_out)
{
    std::string_view raw;
    if (!top_level_member(payload, key, raw)) return false;
    raw = trim_json_ws(raw);
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
    {
        raw.remove_prefix(1);
        raw.remove_suffix(1);
        if (raw.find('\\') != std::string_view::npos) return false;
    }
    else if (raw.empty() || raw.front() == '{' || raw.front() == '['
             || raw == "null" || raw == "true" || raw == "false")
        return false;
    value_out = raw;
    return true;
}

inline bool decision_members_are_unique(
    std::string_view object, std::initializer_list<std::string_view> keys)
{
    for (const auto key : keys)
    {
        std::string_view ignored;
        if (payload_parser(object).inspect_top_level_member(key, ignored)
            == payload_parser::member_result::invalid_or_duplicate)
            return false;
    }
    return true;
}

inline bool has_exact_top_level_code(std::string_view payload, int expected)
{
    std::string_view value;
    if (!top_level_scalar_text(payload, "code", value)) return false;
    char expected_text[24];
    const auto [end, ec] = std::to_chars(
        expected_text, expected_text + sizeof(expected_text), expected);
    return ec == std::errc{}
        && value == std::string_view(expected_text,
                                     static_cast<std::size_t>(end - expected_text));
}

inline bool parse_positive_u64(std::string_view text, std::uint64_t& out)
{
    if (text.empty()) return false;
    std::uint64_t parsed = 0;
    const auto [end, ec] = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc{} || end != text.data() + text.size()
        || parsed == 0)
        return false;
    out = parsed;
    return true;
}

inline bool top_level_positive_u64(std::string_view payload,
                                   std::string_view key,
                                   std::uint64_t& out)
{
    std::string_view text;
    return top_level_scalar_text(payload, key, text)
        && parse_positive_u64(text, out);
}

inline bool is_exact_null(std::string_view value)
{
    while (!value.empty() && std::isspace(
               static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(
               static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value == "null";
}

template <typename Predicate>
inline bool every_top_level_object(
    std::string_view payload, Predicate&& predicate)
{
    if (!is_authoritative_object_array(payload)) return false;
    std::size_t pos = 1;
    while (pos < payload.size())
    {
        while (pos < payload.size()
               && (payload[pos] == ' ' || payload[pos] == '\t'
                   || payload[pos] == '\n' || payload[pos] == '\r'
                   || payload[pos] == ',')) ++pos;
        if (pos >= payload.size() || payload[pos] == ']') return true;

        const auto begin = pos;
        unsigned braces = 0;
        bool in_string = false;
        bool escaped = false;
        for (; pos < payload.size(); ++pos)
        {
            const char c = payload[pos];
            if (in_string)
            {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') { in_string = true; continue; }
            if (c == '{') ++braces;
            else if (c == '}' && --braces == 0)
            {
                ++pos;
                if (!predicate(payload.substr(begin, pos - begin)))
                    return false;
                break;
            }
        }
    }
    return false;
}

} // namespace provider_recovery
