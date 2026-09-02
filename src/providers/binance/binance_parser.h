#pragma once

#include "providers/parser.h"
#include "providers/local/csv_parser.h"
#include "providers/provider_event.h"
#include "providers/recovery_payload.h"
#include "providers/binance/binance_kline_interval.h"
#include "data/date_parse.h"
#include "data/quantity_scale.h"

#include <algorithm>
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
    std::string_view event_time_sv;
    std::string_view trade_id_sv;
    std::string_view maker_raw;
    if (!provider_recovery::is_authoritative_object(json)
        || !provider_recovery::top_level_plain_string(
            json, "e", event_type)
        || event_type != "trade"
        || !provider_recovery::top_level_plain_string(json, "p", price_sv)
        || !provider_recovery::top_level_plain_string(json, "q", qty_sv)
        || !provider_recovery::top_level_plain_string(json, "s", symbol_sv)
        || symbol_sv.empty()
        || !provider_recovery::top_level_scalar_text(json, "E", event_time_sv)
        || !provider_recovery::top_level_scalar_text(json, "t", trade_id_sv)
        || !provider_recovery::top_level_scalar_text(json, "T", time_sv)
        || !provider_recovery::top_level_member(json, "m", maker_raw))
        return std::nullopt;
    maker_raw = provider_recovery::trim_json_ws(maker_raw);
    if (maker_raw != "true" && maker_raw != "false")
        return std::nullopt;
    const bool buyer_is_maker = maker_raw == "true";

    tick_record rec;
    double price = 0.0;
    if (!parse_double_sv(price_sv, price) || !(price > 0.0))
        return std::nullopt;
    const auto qty_atoms =
        tt::quantity_scale::parse_decimal_canonical_atoms(qty_sv);
    if (!qty_atoms || *qty_atoms <= 0)
        return std::nullopt;

    rec.price = price;
    rec.quantity = *qty_atoms;
    rec.quantity_scale = tt::quantity_scale::canonical_atoms;
    rec.symbol.assign(symbol_sv.data(), symbol_sv.size());
    rec.side = buyer_is_maker ? data_tick_side::ask : data_tick_side::bid;

    int64_t ts_ms = 0;
    int64_t event_ms = 0;
    std::uint64_t trade_id = 0;
    const auto [trade_id_end, trade_id_error] = std::from_chars(
        trade_id_sv.data(), trade_id_sv.data() + trade_id_sv.size(), trade_id);
    if (!parse_int64_sv(time_sv, ts_ms)
        || !parse_int64_sv(event_time_sv, event_ms)
        || ts_ms <= 0
        || ts_ms > event_ms
        || trade_id_error != std::errc{}
        || trade_id_end != trade_id_sv.data() + trade_id_sv.size()
        || trade_id == 0)
        return std::nullopt;
    if (!tt::date_parse::from_epoch_milliseconds(ts_ms))
        return std::nullopt;
    const auto timestamp = tt::date_parse::from_epoch_milliseconds(event_ms);
    if (!timestamp)
        return std::nullopt;
    // The engine record has one timestamp. Use authoritative publication
    // time E (known-at/decision time), not occurrence time T, so a strategy
    // cannot observe the trade before its containing frame existed.
    rec.timestamp = *timestamp;

    return rec;
}

// Completed venue bars cross an economic boundary, so both the envelope and
// nested kline object must be authoritative and decision fields unique.
inline std::optional<bar_record> parse_kline_state(
    std::string_view json, bool expected_closed)
{
    std::string_view event_type;
    std::string_view outer_sym_sv;
    std::string_view kline;
    std::string_view open_sv, close_sv, high_sv, low_sv, vol_sv, sym_sv,
        open_time_sv, close_time_sv, event_time_sv, interval_sv, closed_raw;

    if (!provider_recovery::is_authoritative_object(json)
        || !provider_recovery::top_level_plain_string(
            json, "e", event_type)
        || event_type != "kline"
        || !provider_recovery::top_level_plain_string(
            json, "s", outer_sym_sv)
        || outer_sym_sv.empty()
        || !provider_recovery::top_level_scalar_text(
            json, "E", event_time_sv)
        || !provider_recovery::top_level_member(json, "k", kline)
        || !provider_recovery::is_authoritative_object(kline)
        || !provider_recovery::top_level_plain_string(kline, "o", open_sv)
        || !provider_recovery::top_level_plain_string(kline, "c", close_sv)
        || !provider_recovery::top_level_plain_string(kline, "h", high_sv)
        || !provider_recovery::top_level_plain_string(kline, "l", low_sv)
        || !provider_recovery::top_level_plain_string(kline, "v", vol_sv)
        || !provider_recovery::top_level_plain_string(kline, "s", sym_sv)
        || !provider_recovery::top_level_plain_string(kline, "i", interval_sv)
        || !provider_recovery::top_level_scalar_text(
            kline, "t", open_time_sv)
        || !provider_recovery::top_level_scalar_text(
            kline, "T", close_time_sv)
        || !provider_recovery::top_level_member(kline, "x", closed_raw))
        return std::nullopt;
    closed_raw = provider_recovery::trim_json_ws(closed_raw);

    // Binance emits many updates for the currently forming candle. Only x=true
    // is a completed observation suitable for strategy/indicator advancement.
    if (closed_raw != (expected_closed ? "true" : "false")
        || sym_sv.empty() || sym_sv != outer_sym_sv
        || interval_sv.empty())
        return std::nullopt;

    std::int64_t open_time_ms = 0;
    std::int64_t close_time_ms = 0;
    std::int64_t event_time_ms = 0;
    if (!parse_int64_sv(open_time_sv, open_time_ms) || open_time_ms <= 0
        || !parse_int64_sv(close_time_sv, close_time_ms)
        || close_time_ms <= 0 || close_time_ms < open_time_ms
        || !kline_times_match_fixed_interval(
            open_time_ms, close_time_ms, interval_sv)
        || !parse_int64_sv(event_time_sv, event_time_ms)
        || (expected_closed
                ? event_time_ms < close_time_ms
                : (event_time_ms < open_time_ms
                   || event_time_ms > close_time_ms))
        || !tt::date_parse::from_epoch_milliseconds(open_time_ms)
        || !tt::date_parse::from_epoch_milliseconds(close_time_ms)
        || !tt::date_parse::from_epoch_milliseconds(event_time_ms))
        return std::nullopt;

    bar_record rec;
    rec.symbol.assign(sym_sv.data(), sym_sv.size());
    rec.quantity_scale = 100'000'000ULL;

    double v = 0.0;
    if (!parse_double_sv(open_sv, v) || !(v > 0.0)) return std::nullopt;
    rec.open = v;
    if (!parse_double_sv(high_sv, v) || !(v > 0.0)) return std::nullopt;
    rec.high = v;
    if (!parse_double_sv(low_sv, v) || !(v > 0.0)) return std::nullopt;
    rec.low = v;
    if (!parse_double_sv(close_sv, v) || !(v > 0.0)) return std::nullopt;
    rec.close = v;
    if (rec.high < std::max(rec.open, rec.close)
        || rec.low > std::min(rec.open, rec.close)
        || rec.high < rec.low)
        return std::nullopt;
    const auto volume_atoms =
        tt::quantity_scale::parse_decimal_canonical_atoms(vol_sv);
    // A missing/zero volume is not safe in the current bar simulator: zero is
    // its explicit unlimited-liquidity sentinel. Venue bars therefore require
    // a positive, exactly representable economic volume at this boundary.
    if (!volume_atoms
        || (expected_closed ? *volume_atoms <= 0 : *volume_atoms < 0))
        return std::nullopt;
    rec.volume = *volume_atoms;

    rec.open_time_ms = open_time_ms;
    rec.date.assign(event_time_sv.data(), event_time_sv.size());
    return rec;
}

inline std::optional<bar_record> parse_kline(std::string_view json)
{
    return parse_kline_state(json, true);
}

inline bool is_well_formed_forming_kline(std::string_view json)
{
    return parse_kline_state(json, false).has_value();
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
        return parse_record(std::string_view{line});
    }

    std::optional<tick_record> parse_record(std::string_view line) override
    {
        auto parsed = binance::parse_trade(line);
        if (!parsed || (last_timestamp_
                        && parsed->timestamp < *last_timestamp_))
            return std::nullopt;
        last_timestamp_ = parsed->timestamp;
        return parsed;
    }

private:
    std::optional<std::chrono::system_clock::time_point> last_timestamp_;
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
        return parse_record(std::string_view{line});
    }

    std::optional<bar_record> parse_record(std::string_view line) override
    {
        auto parsed = binance::parse_kline(line);
        if (!parsed) return std::nullopt;
        std::int64_t known_ms = 0;
        if (!binance::parse_int64_sv(parsed->date, known_ms)
            || (last_known_ms_ && known_ms < *last_known_ms_))
            return std::nullopt;
        last_known_ms_ = known_ms;
        return parsed;
    }

private:
    std::optional<std::int64_t> last_known_ms_;
};
