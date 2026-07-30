#pragma once
#ifdef HAS_GATE

// Cold-path JSON helpers for Gate wire formats. Hand-rolled only — no
// nlohmann. Expanded for Phase 1 trades / order_book_update / candlesticks.

#include <charconv>
#include <cstddef>
#include <cstdint>
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

// Locate `"key":{` / `"key":[` and return the container slice incl braces.
inline std::string_view extract_container(std::string_view json,
                                          std::string_view key,
                                          char open_char)
{
    auto colon = find_key(json, key);
    if (colon == std::string_view::npos) return {};
    std::size_t pos = colon + 1;
    skip_ws(json, pos);
    if (pos >= json.size() || json[pos] != open_char) return {};
    auto close = matching_close(json, pos);
    if (close == std::string_view::npos) return {};
    return json.substr(pos, close - pos + 1);
}

inline std::string_view extract_object(std::string_view json, std::string_view key)
{
    return extract_container(json, key, '{');
}

inline std::string_view extract_array(std::string_view json, std::string_view key)
{
    return extract_container(json, key, '[');
}

// Iterate top-level objects inside an array value `[ {...}, {...} ]`.
template <typename Fn>
inline void for_each_array_object(std::string_view array, Fn&& fn)
{
    if (array.size() < 2 || array.front() != '[') return;
    std::size_t pos = 1;
    const std::size_t n = array.size();
    while (pos < n)
    {
        skip_ws(array, pos);
        if (pos >= n || array[pos] == ']') break;
        if (array[pos] == ',') { ++pos; continue; }
        if (array[pos] != '{') break;
        auto close = matching_close(array, pos);
        if (close == std::string_view::npos) break;
        fn(array.substr(pos, close - pos + 1));
        pos = close + 1;
    }
}

// Iterate any top-level values (object or array element) in `[ ... ]`.
template <typename Fn>
inline void for_each_array_value(std::string_view array, Fn&& fn)
{
    if (array.size() < 2 || array.front() != '[') return;
    std::size_t pos = 1;
    const std::size_t n = array.size();
    while (pos < n)
    {
        skip_ws(array, pos);
        if (pos >= n || array[pos] == ']') break;
        if (array[pos] == ',') { ++pos; continue; }

        if (array[pos] == '{' || array[pos] == '[')
        {
            auto close = matching_close(array, pos);
            if (close == std::string_view::npos) break;
            fn(array.substr(pos, close - pos + 1));
            pos = close + 1;
            continue;
        }

        // Scalar token (number / string / bool / null).
        if (array[pos] == '"')
        {
            auto end = array.find('"', pos + 1);
            if (end == std::string_view::npos) break;
            fn(array.substr(pos, end - pos + 1));
            pos = end + 1;
            continue;
        }
        auto end = array.find_first_of(",] \t\n\r", pos);
        if (end == std::string_view::npos) end = n;
        fn(array.substr(pos, end - pos));
        pos = end;
    }
}

// Single linear scan of a flat JSON object region [begin, end).
// Nested `{...}` / `[...]` values are returned whole without recursing.
template <typename Fn>
inline void for_each_flat_field(std::string_view json,
                                std::size_t begin,
                                std::size_t end,
                                Fn&& fn)
{
    if (end > json.size()) end = json.size();
    std::size_t pos = begin;
    while (pos < end)
    {
        auto q = json.find('"', pos);
        if (q == std::string_view::npos || q >= end)
            break;

        auto q2 = json.find('"', q + 1);
        if (q2 == std::string_view::npos || q2 >= end)
            break;

        std::string_view key = json.substr(q + 1, q2 - q - 1);
        std::size_t after = q2 + 1;
        skip_ws(json, after);
        if (after >= end || json[after] != ':')
        {
            pos = q + 1;
            continue;
        }

        std::size_t val_pos = after + 1;
        skip_ws(json, val_pos);
        if (val_pos >= end)
            break;

        std::string_view value;
        std::size_t next = val_pos;

        if (json[val_pos] == '"')
        {
            ++val_pos;
            auto vend = json.find('"', val_pos);
            if (vend == std::string_view::npos || vend >= end)
                break;
            value = json.substr(val_pos, vend - val_pos);
            next = vend + 1;
        }
        else if (json[val_pos] == '{' || json[val_pos] == '[')
        {
            auto close = matching_close(json, val_pos);
            if (close == std::string_view::npos || close >= end)
                break;
            value = json.substr(val_pos, close - val_pos + 1);
            next = close + 1;
        }
        else
        {
            auto vend = json.find_first_of(",}]\t\n\r ", val_pos);
            if (vend == std::string_view::npos || vend > end) vend = end;
            value = json.substr(val_pos, vend - val_pos);
            next = vend;
        }

        fn(key, value);
        pos = next;
    }
}

inline bool parse_double_sv(std::string_view sv, double& out)
{
    if (sv.empty()) return false;
    auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc();
}

inline bool parse_int64_sv(std::string_view sv, int64_t& out)
{
    if (sv.empty()) return false;
    auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc();
}

// Parse a JSON number or quoted number into double.
inline bool parse_numberish(std::string_view sv, double& out)
{
    if (sv.size() >= 2 && sv.front() == '"' && sv.back() == '"')
        sv = sv.substr(1, sv.size() - 2);
    return parse_double_sv(sv, out);
}

inline bool parse_intish(std::string_view sv, int64_t& out)
{
    if (sv.size() >= 2 && sv.front() == '"' && sv.back() == '"')
        sv = sv.substr(1, sv.size() - 2);
    // Prefer integer path; fall back to double→int for "1.0".
    if (parse_int64_sv(sv, out)) return true;
    double d = 0.0;
    if (!parse_double_sv(sv, d)) return false;
    out = static_cast<int64_t>(d);
    return true;
}

} // namespace json_util
} // namespace gate

#endif // HAS_GATE
