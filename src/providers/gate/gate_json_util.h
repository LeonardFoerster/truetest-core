#pragma once
#ifdef HAS_GATE

// Minimal cold-path JSON helpers for Gate wire formats (Phase 0 stub).
// Phase 1+ parsers expand this; no nlohmann/json. Not for hot-path
// allocation-sensitive loops without measurement.

#include <cstddef>
#include <string_view>

namespace gate {
namespace json_util {

// Skip ASCII whitespace starting at `pos`.
inline void skip_ws(std::string_view json, std::size_t& pos)
{
    while (pos < json.size()
           && (json[pos] == ' ' || json[pos] == '\t'
               || json[pos] == '\n' || json[pos] == '\r'))
        ++pos;
}

// Find `"key":` and return the index of ':' or npos.
inline std::size_t find_key(std::string_view json, std::string_view key)
{
    if (key.empty() || key.size() > 64)
        return std::string_view::npos;

    // needle = " + key + ":
    char needle[72];
    needle[0] = '"';
    for (std::size_t i = 0; i < key.size(); ++i)
        needle[1 + i] = key[i];
    needle[1 + key.size()] = '"';
    needle[2 + key.size()] = ':';
    const std::size_t nlen = 3 + key.size();

    auto found = json.find(std::string_view(needle, nlen));
    if (found == std::string_view::npos)
        return std::string_view::npos;
    return found + nlen - 1; // index of ':'
}

// After colon: string (unquoted contents) or number/bool/null token.
inline std::string_view value_at_colon(std::string_view json,
                                       std::size_t colon,
                                       std::size_t* out_end = nullptr)
{
    std::size_t pos = colon + 1;
    skip_ws(json, pos);
    if (pos >= json.size())
    {
        if (out_end) *out_end = pos;
        return {};
    }

    if (json[pos] == '"')
    {
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string_view::npos)
        {
            if (out_end) *out_end = json.size();
            return {};
        }
        if (out_end) *out_end = end + 1;
        return json.substr(pos, end - pos);
    }

    auto end = json.find_first_of(",}] \t\n\r", pos);
    if (end == std::string_view::npos) end = json.size();
    if (out_end) *out_end = end;
    return json.substr(pos, end - pos);
}

// First string/number value for key, or empty.
inline std::string_view extract(std::string_view json, std::string_view key)
{
    auto colon = find_key(json, key);
    if (colon == std::string_view::npos) return {};
    return value_at_colon(json, colon);
}

// Matching closing brace/bracket for an object/array starting at `open`
// (open must point at '{' or '['). Returns index of matching closer, or npos.
// Skips string contents so braces inside strings are ignored.
inline std::size_t matching_close(std::string_view json, std::size_t open)
{
    if (open >= json.size()) return std::string_view::npos;
    const char open_ch  = json[open];
    const char close_ch = (open_ch == '{') ? '}'
                        : (open_ch == '[') ? ']'
                        : '\0';
    if (close_ch == '\0') return std::string_view::npos;

    int depth = 0;
    bool in_string = false;
    for (std::size_t i = open; i < json.size(); ++i)
    {
        const char c = json[i];
        if (in_string)
        {
            if (c == '\\' && i + 1 < json.size())
            {
                ++i; // skip escaped char
                continue;
            }
            if (c == '"') in_string = false;
            continue;
        }
        if (c == '"')
        {
            in_string = true;
            continue;
        }
        if (c == open_ch)
        {
            ++depth;
            continue;
        }
        if (c == close_ch)
        {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string_view::npos;
}

} // namespace json_util
} // namespace gate

#endif // HAS_GATE
