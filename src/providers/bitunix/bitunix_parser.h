#pragma once
#ifdef HAS_BITUNIX

// Hand-rolled Bitunix public WS parsers (no nlohmann on this path).
// Trade push shape:
//   {"ch":"trade","symbol":"BTCUSDT","ts":...,"data":[{"p":"...","v":"...","s":"buy","t":"..."},...]}

#include "providers/parser.h"
#include "providers/provider_event.h"
#include "providers/local/csv_parser.h"
#include "providers/recovery_payload.h"
#include "data/quantity_scale.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bitunix {

namespace detail {

inline void skip_ws(std::string_view j, std::size_t& pos)
{
    while (pos < j.size() &&
           (j[pos] == ' ' || j[pos] == '\t' || j[pos] == '\n' || j[pos] == '\r'))
        ++pos;
}

inline std::size_t find_key(std::string_view json, std::string_view key)
{
    if (key.empty() || key.size() > 64)
        return std::string_view::npos;
    char needle[72];
    needle[0] = '"';
    std::memcpy(needle + 1, key.data(), key.size());
    needle[1 + key.size()] = '"';
    needle[2 + key.size()] = ':';
    const std::size_t nlen = 3 + key.size();
    auto found = json.find(std::string_view(needle, nlen));
    if (found == std::string_view::npos)
        return std::string_view::npos;
    return found + nlen - 1;
}

inline std::optional<std::string_view> extract_string(std::string_view json,
                                                      std::string_view key)
{
    auto colon = find_key(json, key);
    if (colon == std::string_view::npos)
        return std::nullopt;
    std::size_t pos = colon + 1;
    skip_ws(json, pos);
    if (pos >= json.size() || json[pos] != '"')
        return std::nullopt;
    ++pos;
    const std::size_t start = pos;
    while (pos < json.size() && json[pos] != '"')
    {
        if (json[pos] == '\\' && pos + 1 < json.size())
            pos += 2;
        else
            ++pos;
    }
    if (pos >= json.size())
        return std::nullopt;
    return json.substr(start, pos - start);
}

inline std::optional<double> parse_double_token(std::string_view tok)
{
    if (tok.empty())
        return std::nullopt;
    // Prefer strtod: from_chars(float) is incomplete on some libstdc++ builds.
    std::string tmp(tok);
    char* end = nullptr;
    const double v = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str() || *end != '\0' || !std::isfinite(v))
        return std::nullopt;
    return v;
}

inline std::optional<double> extract_number(std::string_view json,
                                            std::string_view key)
{
    auto colon = find_key(json, key);
    if (colon == std::string_view::npos)
        return std::nullopt;
    std::size_t pos = colon + 1;
    skip_ws(json, pos);
    if (pos < json.size() && json[pos] == '"')
    {
        ++pos;
        const std::size_t start = pos;
        while (pos < json.size() && json[pos] != '"')
            ++pos;
        if (pos <= start)
            return std::nullopt;
        return parse_double_token(json.substr(start, pos - start));
    }
    const std::size_t start = pos;
    if (pos < json.size() && (json[pos] == '-' || json[pos] == '+'))
        ++pos;
    while (pos < json.size() &&
           ((json[pos] >= '0' && json[pos] <= '9') || json[pos] == '.' ||
            json[pos] == 'e' || json[pos] == 'E' || json[pos] == '+' ||
            json[pos] == '-'))
        ++pos;
    if (pos <= start)
        return std::nullopt;
    return parse_double_token(json.substr(start, pos - start));
}

inline std::optional<std::int64_t> extract_int(std::string_view json,
                                               std::string_view key)
{
    auto n = extract_number(json, key);
    if (!n || *n < static_cast<double>(std::numeric_limits<std::int64_t>::min())
        || *n > static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    return static_cast<std::int64_t>(*n);
}

// Locate "data":[ ... ] array body (best-effort bracket match).
inline std::optional<std::string_view> extract_data_array(std::string_view json)
{
    auto colon = find_key(json, "data");
    if (colon == std::string_view::npos)
        return std::nullopt;
    std::size_t pos = colon + 1;
    skip_ws(json, pos);
    if (pos >= json.size() || json[pos] != '[')
        return std::nullopt;
    const std::size_t start = pos;
    int depth = 0;
    bool in_str = false;
    for (; pos < json.size(); ++pos)
    {
        const char c = json[pos];
        if (in_str)
        {
            if (c == '\\' && pos + 1 < json.size())
            {
                ++pos;
                continue;
            }
            if (c == '"')
                in_str = false;
            continue;
        }
        if (c == '"')
        {
            in_str = true;
            continue;
        }
        if (c == '[')
            ++depth;
        else if (c == ']')
        {
            --depth;
            if (depth == 0)
                return json.substr(start, pos - start + 1);
        }
    }
    return std::nullopt;
}

// Iterate object elements inside a JSON array of objects.
inline void for_each_object(std::string_view arr,
                            const std::function<void(std::string_view)>& fn)
{
    if (arr.empty() || arr.front() != '[')
        return;
    std::size_t pos = 1;
    while (pos < arr.size())
    {
        skip_ws(arr, pos);
        if (pos >= arr.size() || arr[pos] == ']')
            break;
        if (arr[pos] != '{')
        {
            ++pos;
            continue;
        }
        const std::size_t start = pos;
        int depth = 0;
        bool in_str = false;
        for (; pos < arr.size(); ++pos)
        {
            const char c = arr[pos];
            if (in_str)
            {
                if (c == '\\' && pos + 1 < arr.size())
                {
                    ++pos;
                    continue;
                }
                if (c == '"')
                    in_str = false;
                continue;
            }
            if (c == '"')
            {
                in_str = true;
                continue;
            }
            if (c == '{')
                ++depth;
            else if (c == '}')
            {
                --depth;
                if (depth == 0)
                {
                    fn(arr.substr(start, pos - start + 1));
                    ++pos;
                    break;
                }
            }
        }
        skip_ws(arr, pos);
        if (pos < arr.size() && arr[pos] == ',')
            ++pos;
    }
}

inline std::chrono::system_clock::time_point ms_to_tp(std::int64_t ms)
{
    using namespace std::chrono;
    return system_clock::time_point{milliseconds{ms}};
}

// ISO-ish "2026-04-07T05:47:52Z" → epoch ms (best-effort; 0 on failure).
inline std::int64_t parse_iso_ms(std::string_view t)
{
    // Prefer outer frame ts when available; this is a fallback.
    if (t.size() < 19)
        return 0;
    // Minimal parse: YYYY-MM-DDTHH:MM:SS
    int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
    if (std::sscanf(t.data(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6)
        return 0;
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = s;
#if defined(_WIN32)
    const auto sec = _mkgmtime(&tm);
#else
    const auto sec = timegm(&tm);
#endif
    if (sec < 0)
        return 0;
    return static_cast<std::int64_t>(sec) * 1000;
}

} // namespace detail

inline std::string extract_channel(std::string_view json)
{
    if (auto ch = detail::extract_string(json, "ch"))
        return std::string(*ch);
    return {};
}

// Parse all trades in a public trade push into tick_records.
inline std::vector<tick_record> parse_all_trades(std::string_view json)
{
    std::vector<tick_record> out;
    auto arr = detail::extract_data_array(json);
    if (!arr)
        return out;

    std::string symbol;
    if (auto s = detail::extract_string(json, "symbol"))
        symbol = std::string(*s);

    std::int64_t frame_ts = 0;
    if (auto ts = detail::extract_int(json, "ts"))
        frame_ts = *ts;

    detail::for_each_object(*arr, [&](std::string_view obj) {
        // Prefer string fields (Bitunix quotes p/v); fall back to bare numbers.
        std::optional<double> p;
        if (auto ps = detail::extract_string(obj, "p"))
            p = detail::parse_double_token(*ps);
        if (!p)
            p = detail::extract_number(obj, "p");

        std::optional<double> v;
        if (auto vs = detail::extract_string(obj, "v"))
            v = detail::parse_double_token(*vs);
        if (!v)
            v = detail::extract_number(obj, "v");

        if (!p || !v || *p <= 0.0 || *v <= 0.0)
            return;
        std::int64_t qty_atoms = 0;
        if (!tt::quantity_scale::from_base_nonnegative(
                *v, tt::quantity_scale::canonical_atoms, qty_atoms)
            || qty_atoms <= 0)
            return;
        tick_record rec;
        rec.price = *p;
        // Domain Tick uses fixed-scale int64 qty (1e8), same as Binance/Bitget.
        rec.quantity = qty_atoms;
        rec.quantity_scale = tt::quantity_scale::canonical_atoms;
        rec.symbol = symbol;
        if (auto side = detail::extract_string(obj, "s"))
        {
            if (*side == "buy" || *side == "BUY")
                rec.side = data_tick_side::bid;
            else if (*side == "sell" || *side == "SELL")
                rec.side = data_tick_side::ask;
            else
                rec.side = data_tick_side::unknown;
        }
        std::int64_t ms = frame_ts;
        if (auto t = detail::extract_string(obj, "t"))
        {
            auto parsed = detail::parse_iso_ms(*t);
            if (parsed > 0)
                ms = parsed;
        }
        if (ms > 0)
            rec.timestamp = detail::ms_to_tp(ms);
        else
            rec.timestamp = std::chrono::system_clock::now();
        out.push_back(std::move(rec));
    });
    return out;
}

inline std::optional<tick_record> parse_trade_first(std::string_view json)
{
    auto all = parse_all_trades(json);
    if (all.empty())
        return std::nullopt;
    return all.front();
}

inline provider::tick to_provider_tick(const tick_record& rec)
{
    provider::tick t;
    t.timestamp = rec.timestamp;
    t.symbol = rec.symbol;
    t.price = rec.price;
    t.quantity = rec.quantity;
    t.quantity_scale = rec.quantity_scale;
    t.side = (rec.side == data_tick_side::bid) ? 0
           : (rec.side == data_tick_side::ask) ? 1 : 2;
    return t;
}

} // namespace bitunix

// ---------------------------------------------------------------------------
// IDataParser adapters
// ---------------------------------------------------------------------------

class BitunixTradeParser : public IDataParser<tick_record>
{
public:
    std::optional<tick_record> parse_record(const std::string& line) override
    {
        return parse_record(std::string_view{line});
    }

    std::optional<tick_record> parse_record(std::string_view line) override
    {
        return bitunix::parse_trade_first(line);
    }

    std::vector<tick_record> parse_records(std::string_view line) override
    {
        return bitunix::parse_all_trades(line);
    }
};

class BitunixCombinedParser : public IDataParser<provider::event>
{
public:
    std::optional<provider::event> parse_record(const std::string& line) override
    {
        return parse_record(std::string_view{line});
    }

    std::optional<provider::event> parse_record(std::string_view line) override
    {
        auto batch = parse_records(line);
        if (batch.empty())
            return std::nullopt;
        return batch.front();
    }

    std::vector<provider::event> parse_records(std::string_view line) override
    {
        std::vector<provider::event> out;
        // Ignore non-trade control frames (pong / subscribe acks).
        const auto ch = bitunix::extract_channel(line);
        if (!ch.empty() && ch != "trade")
            return out;

        // pong / op responses have no data[] trades
        if (line.find("\"op\"") != std::string_view::npos &&
            line.find("\"data\"") == std::string_view::npos)
            return out;

        for (auto& rec : bitunix::parse_all_trades(line))
            out.emplace_back(bitunix::to_provider_tick(rec));
        return out;
    }

    empty_parse_status classify_empty_frame(std::string_view line) const override
    {
        if (!provider_recovery::is_authoritative_object(line)
            || !provider_recovery::decision_members_are_unique(
                line, {"ch", "op", "data"}))
            return empty_parse_status::malformed;
        const auto ch = bitunix::extract_channel(line);
        std::string_view data;
        const bool no_data = provider_recovery::payload_parser(line)
            .inspect_top_level_member("data", data)
            == provider_recovery::payload_parser::member_result::missing;
        auto op = bitunix::detail::extract_string(line, "op");
        if ((!ch.empty() && ch != "trade")
            || (no_data && op && (*op == "pong" || *op == "subscribe")))
            return empty_parse_status::ignored;
        return empty_parse_status::malformed;
    }
};

#endif // HAS_BITUNIX
