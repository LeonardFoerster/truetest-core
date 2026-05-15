#pragma once

#include "providers/parser.h"
#include "providers/local/csv_parser.h"
#include "providers/provider_event.h"

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>


namespace binance {


namespace detail {

inline std::size_t find_key(std::string_view json, std::string_view key)
{
    std::size_t pos = 0;
    while (pos < json.size())
    {
        auto q = json.find('"', pos);
        if (q == std::string_view::npos) return std::string_view::npos;
        if (q + 1 + key.size() + 1 > json.size())
            return std::string_view::npos;
        if (json[q + 1 + key.size()] == '"' &&
            json.compare(q + 1, key.size(), key) == 0)
        {
            std::size_t after = q + 1 + key.size() + 1;
            while (after < json.size() && (json[after] == ' ' || json[after] == '\t'))
                ++after;
            if (after < json.size() && json[after] == ':')
                return after;
        }
        pos = q + 1;
    }
    return std::string_view::npos;
}

inline void skip_ws(std::string_view json, std::size_t& pos)
{
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        ++pos;
}

}

inline std::string_view extract_sv_string(std::string_view json, std::string_view key)
{
    auto colon = detail::find_key(json, key);
    if (colon == std::string_view::npos) return {};
    std::size_t pos = colon + 1;
    detail::skip_ws(json, pos);
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;
    auto end = json.find('"', pos);
    if (end == std::string_view::npos) return {};
    return json.substr(pos, end - pos);
}

inline std::string_view extract_sv_number(std::string_view json, std::string_view key)
{
    auto colon = detail::find_key(json, key);
    if (colon == std::string_view::npos) return {};
    std::size_t pos = colon + 1;
    detail::skip_ws(json, pos);
    if (pos >= json.size()) return {};

    if (json[pos] == '"')
    {
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string_view::npos) return {};
        return json.substr(pos, end - pos);
    }
    auto end = json.find_first_of(",}] \t\n\r", pos);
    if (end == std::string_view::npos) end = json.size();
    return json.substr(pos, end - pos);
}

inline bool extract_sv_bool(std::string_view json, std::string_view key)
{
    auto colon = detail::find_key(json, key);
    if (colon == std::string_view::npos) return false;
    std::size_t pos = colon + 1;
    detail::skip_ws(json, pos);
    // Require an exact `true` literal (lowercase, JSON spec). The previous
    // version returned true on any value starting with `t`, which would
    // mis-classify e.g. `"truncated"` as boolean true.
    if (pos + 4 > json.size()) return false;
    return json.compare(pos, 4, "true") == 0;
}

// Distinguishes "exactly true", "exactly false", and "missing or
// malformed". Callers that proceed on an unknown response (e.g.
// position-mode safety gate) should use this and refuse on nullopt.
inline std::optional<bool> extract_sv_optional_bool(std::string_view json,
                                                    std::string_view key)
{
    auto colon = detail::find_key(json, key);
    if (colon == std::string_view::npos) return std::nullopt;
    std::size_t pos = colon + 1;
    detail::skip_ws(json, pos);
    if (pos + 4 <= json.size() && json.compare(pos, 4, "true") == 0)
        return true;
    if (pos + 5 <= json.size() && json.compare(pos, 5, "false") == 0)
        return false;
    return std::nullopt;
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

inline std::optional<tick_record> parse_trade(std::string_view json)
{
    auto event_type = extract_sv_string(json, "e");
    if (event_type != "trade") return std::nullopt;

    auto price_sv  = extract_sv_string(json, "p");
    auto qty_sv    = extract_sv_string(json, "q");
    auto symbol_sv = extract_sv_string(json, "s");
    auto time_sv   = extract_sv_number(json, "T");

    if (price_sv.empty() || qty_sv.empty() || symbol_sv.empty())
        return std::nullopt;

    tick_record rec;
    double price = 0.0, qty = 0.0;
    if (!parse_double_sv(price_sv, price)) return std::nullopt;
    if (!parse_double_sv(qty_sv, qty)) return std::nullopt;

    rec.price = price;
    rec.quantity = static_cast<int64_t>(qty * 1e8);
    rec.symbol.assign(symbol_sv.data(), symbol_sv.size());

    bool buyer_is_maker = extract_sv_bool(json, "m");
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

inline std::optional<bar_record> parse_kline(std::string_view json)
{
    auto event_type = extract_sv_string(json, "e");
    if (event_type != "kline") return std::nullopt;

    auto k_pos = json.find("\"k\":{");
    if (k_pos == std::string_view::npos) return std::nullopt;
    std::string_view k_json = json.substr(k_pos);

    auto open_sv  = extract_sv_string(k_json, "o");
    auto close_sv = extract_sv_string(k_json, "c");
    auto high_sv  = extract_sv_string(k_json, "h");
    auto low_sv   = extract_sv_string(k_json, "l");
    auto vol_sv   = extract_sv_string(k_json, "v");
    auto sym_sv   = extract_sv_string(k_json, "s");

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

    auto time_sv = extract_sv_number(k_json, "t");
    rec.date.assign(time_sv.data(), time_sv.size());

    return rec;
}

}

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
