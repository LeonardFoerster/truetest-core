#pragma once

#include "providers/parser.h"
#include "providers/local/csv_parser.h"
#include "providers/provider_event.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace binance {

namespace detail {

inline void skip_ws(std::string_view json, std::size_t& pos)
{
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        ++pos;
}

// Locate `"key":` via a single memchr-style find of the full needle.
// Returns the index of ':' or npos. Much faster than scanning every quote
// and re-checking the key (old path: ~62 ns/key × N fields).
//
// Semantics: first occurrence of the exact pattern `"key":` (no space between
// closing quote and colon — matches Binance wire format). Callers that need
// to tolerate spaces after the key use the skip_ws after the colon.
inline std::size_t find_key(std::string_view json, std::string_view key)
{
    if (key.empty() || key.size() > 64)
        return std::string_view::npos;

    // needle = " + key + ":
    char needle[72];
    needle[0] = '"';
    std::memcpy(needle + 1, key.data(), key.size());
    needle[1 + key.size()] = '"';
    needle[2 + key.size()] = ':';
    const std::size_t nlen = 3 + key.size();

    auto found = json.find(std::string_view(needle, nlen));
    if (found == std::string_view::npos)
        return std::string_view::npos;
    return found + nlen - 1; // index of ':'
}

// After colon: return value as string_view (string or number/bool token).
// Optionally writes the position just past the value into *out_end.
inline std::string_view value_at_colon(std::string_view json, std::size_t colon,
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

    // number / true / false / null
    auto end = json.find_first_of(",}] \t\n\r", pos);
    if (end == std::string_view::npos) end = json.size();
    if (out_end) *out_end = end;
    return json.substr(pos, end - pos);
}

// Single linear scan of a flat JSON object region [begin, end).
// Invokes fn(key, value_sv) for every `"key":value` pair.
// Skips nested `{...}` / `[...]` values without recursing into their keys
// (sufficient for Binance trade / k-object fields).
template <typename Fn>
inline void for_each_flat_field(std::string_view json, std::size_t begin, std::size_t end,
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
            // Quote was inside a value, not a key — advance one char.
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
            // Skip nested structure (e.g. outer wrapper before "k").
            const char open = json[val_pos];
            const char close = (open == '{') ? '}' : ']';
            int depth = 0;
            std::size_t i = val_pos;
            for (; i < end; ++i)
            {
                const char c = json[i];
                if (c == '"')
                {
                    // Skip string contents so braces inside strings don't
                    // confuse depth counting.
                    ++i;
                    while (i < end && json[i] != '"')
                    {
                        if (json[i] == '\\' && i + 1 < end) ++i;
                        ++i;
                    }
                    continue;
                }
                if (c == open) ++depth;
                else if (c == close)
                {
                    --depth;
                    if (depth == 0) { ++i; break; }
                }
            }
            value = json.substr(val_pos, i - val_pos);
            next = i;
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

// Hash a short key (1–2 chars typical) for switch tables — not cryptographic.
inline constexpr unsigned key_tag(std::string_view k)
{
    unsigned h = 0;
    for (unsigned char c : k)
        h = (h << 8) | c;
    return h;
}

} // namespace detail

inline std::string_view extract_sv_string(std::string_view json, std::string_view key)
{
    auto colon = detail::find_key(json, key);
    if (colon == std::string_view::npos) return {};
    std::size_t pos = colon + 1;
    detail::skip_ws(json, pos);
    // Only accept JSON string values (quoted).
    if (pos >= json.size() || json[pos] != '"') return {};
    return detail::value_at_colon(json, colon);
}

inline std::string_view extract_sv_number(std::string_view json, std::string_view key)
{
    auto colon = detail::find_key(json, key);
    if (colon == std::string_view::npos) return {};
    return detail::value_at_colon(json, colon);
}

inline bool extract_sv_bool(std::string_view json, std::string_view key)
{
    auto colon = detail::find_key(json, key);
    if (colon == std::string_view::npos) return false;
    auto sv = detail::value_at_colon(json, colon);
    return sv.size() == 4 && sv == "true";
}

// Distinguishes "exactly true", "exactly false", and "missing or malformed".
inline std::optional<bool> extract_sv_optional_bool(std::string_view json,
                                                    std::string_view key)
{
    auto colon = detail::find_key(json, key);
    if (colon == std::string_view::npos) return std::nullopt;
    auto sv = detail::value_at_colon(json, colon);
    if (sv == "true") return true;
    if (sv == "false") return false;
    return std::nullopt;
}

inline bool parse_double_sv(std::string_view sv, double& out)
{
    if (sv.empty()) return false;
    auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc() && p == sv.data() + sv.size()
        && std::isfinite(out);
}

inline bool parse_int64_sv(std::string_view sv, int64_t& out)
{
    if (sv.empty()) return false;
    auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc() && p == sv.data() + sv.size();
}

inline std::string extract_string(const std::string& json, const std::string& key)
{
    auto sv = extract_sv_string(json, key);
    return std::string(sv);
}

inline std::string extract_number(const std::string& json, const std::string& key)
{
    auto sv = extract_sv_number(json, key);
    return std::string(sv);
}

inline bool extract_bool(const std::string& json, const std::string& key)
{
    return extract_sv_bool(json, key);
}

// Hot path: one linear scan for all trade fields (was 6–7× full find_key).
inline std::optional<tick_record> parse_trade(std::string_view json)
{
    std::string_view event_type;
    std::string_view price_sv;
    std::string_view qty_sv;
    std::string_view symbol_sv;
    std::string_view time_sv;
    bool buyer_is_maker = false;
    bool saw_m = false;

    detail::for_each_flat_field(json, 0, json.size(),
        [&](std::string_view key, std::string_view value) {
            switch (detail::key_tag(key))
            {
            case detail::key_tag("e"): event_type = value; break;
            case detail::key_tag("p"): price_sv   = value; break;
            case detail::key_tag("q"): qty_sv     = value; break;
            case detail::key_tag("s"): symbol_sv  = value; break;
            case detail::key_tag("T"): time_sv    = value; break;
            case detail::key_tag("m"):
                buyer_is_maker = (value == "true");
                saw_m = true;
                break;
            default: break;
            }
        });

    (void)saw_m;
    if (event_type != "trade") return std::nullopt;
    if (price_sv.empty() || qty_sv.empty() || symbol_sv.empty())
        return std::nullopt;

    tick_record rec;
    double price = 0.0, qty = 0.0;
    if (!parse_double_sv(price_sv, price)) return std::nullopt;
    if (!parse_double_sv(qty_sv, qty)) return std::nullopt;

    rec.price = price;
    rec.quantity = static_cast<int64_t>(qty * 1e8);
    rec.symbol.assign(symbol_sv.data(), symbol_sv.size());
    rec.side = buyer_is_maker ? data_tick_side::ask : data_tick_side::bid;

    int64_t ts_ms = 0;
    if (parse_int64_sv(time_sv, ts_ms))
    {
        rec.timestamp = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(ts_ms));
    }
    else
    {
        rec.timestamp = std::chrono::system_clock::now();
    }

    return rec;
}

// Hot path: locate k-object once, then single-pass its fields.
inline std::optional<bar_record> parse_kline(std::string_view json)
{
    // Quick reject without full scan when e is present and not kline.
    // (Single-pass still needed for k fields; this avoids work on wrong types.)
    {
        auto e_colon = detail::find_key(json, "e");
        if (e_colon != std::string_view::npos)
        {
            auto e = detail::value_at_colon(json, e_colon);
            if (e != "kline") return std::nullopt;
        }
        else
        {
            return std::nullopt;
        }
    }

    // Locate the k-object. Prefer the tight form `"k":{` (wire format);
    // fall back to `"k":` + skip_ws + `{` for pretty-printed fixtures.
    std::size_t brace = std::string_view::npos;
    {
        auto tight = json.find("\"k\":{");
        if (tight != std::string_view::npos)
        {
            brace = tight + 4; // index of '{'
        }
        else
        {
            auto k_colon = detail::find_key(json, "k");
            if (k_colon == std::string_view::npos) return std::nullopt;
            std::size_t pos = k_colon + 1;
            detail::skip_ws(json, pos);
            if (pos >= json.size() || json[pos] != '{') return std::nullopt;
            brace = pos;
        }
    }

    // Find matching close for k object
    std::size_t k_end = brace;
    {
        int depth = 0;
        for (std::size_t i = brace; i < json.size(); ++i)
        {
            const char c = json[i];
            if (c == '"')
            {
                ++i;
                while (i < json.size() && json[i] != '"')
                {
                    if (json[i] == '\\' && i + 1 < json.size()) ++i;
                    ++i;
                }
                continue;
            }
            if (c == '{') ++depth;
            else if (c == '}')
            {
                --depth;
                if (depth == 0) { k_end = i; break; }
            }
        }
    }

    std::string_view open_sv, close_sv, high_sv, low_sv, vol_sv, sym_sv, time_sv;

    detail::for_each_flat_field(json, brace + 1, k_end,
        [&](std::string_view key, std::string_view value) {
            switch (detail::key_tag(key))
            {
            case detail::key_tag("o"): open_sv  = value; break;
            case detail::key_tag("c"): close_sv = value; break;
            case detail::key_tag("h"): high_sv  = value; break;
            case detail::key_tag("l"): low_sv   = value; break;
            case detail::key_tag("v"): vol_sv   = value; break;
            case detail::key_tag("s"): sym_sv   = value; break;
            case detail::key_tag("t"): time_sv  = value; break;
            default: break;
            }
        });

    if (open_sv.empty() || close_sv.empty() || high_sv.empty() || low_sv.empty())
        return std::nullopt;

    bar_record rec;
    rec.symbol.assign(sym_sv.data(), sym_sv.size());

    double v = 0.0;
    if (!parse_double_sv(open_sv, v))  return std::nullopt;
    rec.open = v;
    if (!parse_double_sv(high_sv, v))  return std::nullopt;
    rec.high = v;
    if (!parse_double_sv(low_sv, v))   return std::nullopt;
    rec.low = v;
    if (!parse_double_sv(close_sv, v)) return std::nullopt;
    rec.close = v;
    if (!vol_sv.empty() && parse_double_sv(vol_sv, v))
        rec.volume = static_cast<int64_t>(v * 1e8);
    else
        rec.volume = 0;

    rec.date.assign(time_sv.data(), time_sv.size());
    return rec;
}

} // namespace binance

class BinanceTradeParser : public IDataParser<tick_record>
{
public:
    bool parse_header(const std::string&) override
    {
        return true;
    }

    std::optional<tick_record> parse_record(const std::string& line) override
    {
        return binance::parse_trade(std::string_view{line});
    }

    std::optional<tick_record> parse_record(std::string_view line) override
    {
        return binance::parse_trade(line);
    }
};

class BinanceKlineParser : public IDataParser<bar_record>
{
public:
    bool parse_header(const std::string&) override
    {
        return true;
    }

    std::optional<bar_record> parse_record(const std::string& line) override
    {
        return binance::parse_kline(std::string_view{line});
    }

    std::optional<bar_record> parse_record(std::string_view line) override
    {
        return binance::parse_kline(line);
    }
};
