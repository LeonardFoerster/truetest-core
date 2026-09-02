#pragma once
#ifdef HAS_BITGET

#include "data/data_handler.h"
#include "data/date_parse.h"
#include "data/quantity_scale.h"
#include "providers/local/csv_parser.h"
#include "providers/parser.h"
#include "providers/provider_event.h"
#include "providers/recovery_payload.h"

#include <charconv>
#include <cmath>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bitget {

namespace detail {

inline void skip_ws(std::string_view json, std::size_t& pos)
{
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        ++pos;
}

// Locate `"key":` via a single find of the full needle. Returns index of ':'
// or npos. Matches Bitget wire (no space between quote and colon).
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
    return found + nlen - 1; // index of ':'
}

// After colon: return value as string_view (string or number/bool token).
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

    auto end = json.find_first_of(",}] \t\n\r", pos);
    if (end == std::string_view::npos) end = json.size();
    if (out_end) *out_end = end;
    return json.substr(pos, end - pos);
}

// Single linear scan of a flat JSON object region [begin, end).
// Nested `{...}` / `[...]` values are returned whole without recursing.
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
            const char open = json[val_pos];
            const char close = (open == '{') ? '}' : ']';
            int depth = 0;
            std::size_t i = val_pos;
            for (; i < end; ++i)
            {
                const char c = json[i];
                if (c == '"')
                {
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

// Span of a balanced {...} or [...] starting at open brace/bracket.
inline std::size_t match_container(std::string_view json, std::size_t open)
{
    if (open >= json.size()) return std::string_view::npos;
    const char open_c = json[open];
    if (open_c != '{' && open_c != '[') return std::string_view::npos;
    const char close_c = (open_c == '{') ? '}' : ']';
    int depth = 0;
    for (std::size_t i = open; i < json.size(); ++i)
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
        if (c == open_c) ++depth;
        else if (c == close_c)
        {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string_view::npos;
}

// Locate `"key":{` / `"key":[` (optional space) and return the container
// slice including braces/brackets.
inline std::string_view extract_container(std::string_view json, std::string_view key,
                                          char open_char)
{
    auto colon = find_key(json, key);
    if (colon == std::string_view::npos) return {};
    std::size_t pos = colon + 1;
    skip_ws(json, pos);
    if (pos >= json.size() || json[pos] != open_char) return {};
    auto close = match_container(json, pos);
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

inline std::string_view top_level_object(std::string_view json,
                                         std::string_view key)
{
    std::string_view value;
    if (!provider_recovery::top_level_member(json, key, value)) return {};
    value = provider_recovery::trim_json_ws(value);
    return provider_recovery::is_authoritative_object(value)
        ? value : std::string_view{};
}

inline std::string_view top_level_array(std::string_view json,
                                        std::string_view key)
{
    std::string_view value;
    if (!provider_recovery::top_level_member(json, key, value)) return {};
    value = provider_recovery::trim_json_ws(value);
    return provider_recovery::is_authoritative_object_array(value)
        ? value : std::string_view{};
}

inline bool authoritative_public_envelope(std::string_view json)
{
    if (!provider_recovery::is_authoritative_object(json)
        || !provider_recovery::decision_members_are_unique(
            json, {"arg", "data", "symbol", "action", "ts"}))
        return false;

    std::string_view member;
    const auto arg_result = provider_recovery::payload_parser(json)
        .inspect_top_level_member("arg", member);
    if (arg_result
        == provider_recovery::payload_parser::member_result::invalid_or_duplicate
        || (arg_result == provider_recovery::payload_parser::member_result::unique
            && !provider_recovery::is_authoritative_object(member)))
        return false;

    const auto data_result = provider_recovery::payload_parser(json)
        .inspect_top_level_member("data", member);
    return data_result
            != provider_recovery::payload_parser::member_result::invalid_or_duplicate
        && (data_result == provider_recovery::payload_parser::member_result::missing
            || provider_recovery::is_authoritative_object_array(member));
}

inline bool authoritative_arg(std::string_view arg)
{
    return arg.empty()
        || (provider_recovery::is_authoritative_object(arg)
            && provider_recovery::decision_members_are_unique(
                arg, {"instType", "topic", "symbol", "interval"}));
}

inline bool authoritative_usdt_public_trade(std::string_view envelope,
                                            std::string_view arg)
{
    if (arg.empty()) return false;
    std::string_view inst_type;
    std::string_view topic;
    if (!provider_recovery::top_level_plain_string(
            arg, "instType", inst_type)
        || inst_type != "usdt-futures"
        || !provider_recovery::top_level_plain_string(arg, "topic", topic)
        || topic != "publicTrade")
        return false;

    std::string_view action;
    const auto action_state = provider_recovery::payload_parser(envelope)
        .inspect_top_level_member("action", action);
    if (action_state
        == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
        return false;
    if (action_state == provider_recovery::payload_parser::member_result::unique)
    {
        if (!provider_recovery::top_level_plain_string(
                envelope, "action", action)
            || (action != "snapshot" && action != "update"))
            return false;
    }
    return true;
}

inline bool at_most_one_top_level_member(
    std::string_view object,
    std::initializer_list<std::string_view> aliases)
{
    unsigned present = 0;
    for (const auto alias : aliases)
    {
        std::string_view value;
        const auto state = provider_recovery::payload_parser(object)
            .inspect_top_level_member(alias, value);
        if (state
            == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
            return false;
        if (state == provider_recovery::payload_parser::member_result::unique)
            ++present;
    }
    return present <= 1;
}

inline bool resolve_public_symbol(std::string_view envelope,
                                  std::string_view arg,
                                  std::string_view& symbol)
{
    std::string_view arg_symbol;
    std::string_view outer_symbol;
    if (!arg.empty())
        (void)provider_recovery::top_level_plain_string(
            arg, "symbol", arg_symbol);
    (void)provider_recovery::top_level_plain_string(
        envelope, "symbol", outer_symbol);
    if (!arg_symbol.empty() && !outer_symbol.empty()
        && arg_symbol != outer_symbol)
        return false;
    symbol = !arg_symbol.empty() ? arg_symbol : outer_symbol;
    return !symbol.empty();
}

// Iterate top-level objects inside an array value `[ {...}, {...} ]`.
template <typename Fn>
inline bool for_each_array_object(std::string_view array, Fn&& fn)
{
    if (array.size() < 2 || array.front() != '[') return false;
    std::size_t pos = 1;
    const std::size_t n = array.size();
    bool need_object = true;
    bool saw_object = false;
    while (pos < n)
    {
        skip_ws(array, pos);
        if (pos >= n) return false;
        if (array[pos] == ']')
            return !need_object || !saw_object;
        if (!need_object || array[pos] != '{') return false;
        auto close = match_container(array, pos);
        if (close == std::string_view::npos) return false;
        fn(array.substr(pos, close - pos + 1));
        saw_object = true;
        need_object = false;
        pos = close + 1;
        skip_ws(array, pos);
        if (pos >= n) return false;
        if (array[pos] == ']') return true;
        if (array[pos] != ',') return false;
        ++pos;
        need_object = true;
    }
    return false;
}

inline std::string_view first_data_object(std::string_view json)
{
    auto arr = top_level_array(json, "data");
    if (arr.empty()) return {};
    std::string_view first;
    std::size_t count = 0;
    const bool valid = for_each_array_object(arr, [&](std::string_view obj) {
        ++count;
        if (first.empty()) first = obj;
    });
    return valid && count == 1 ? first : std::string_view{};
}

// Locate `"key":[` and append levels into out. Qty scaled *1e8 like Binance.
inline bool append_levels(std::string_view json, std::string_view key,
                          std::vector<provider::l2_snapshot::level>& out)
{
    std::string_view arr;
    const auto result = provider_recovery::payload_parser(json)
        .inspect_top_level_member(key, arr);
    if (result
        == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
        return false;
    if (result == provider_recovery::payload_parser::member_result::missing)
        return true;
    arr = provider_recovery::trim_json_ws(arr);
    if (arr.size() < 2 || arr.front() != '[' || arr.back() != ']')
        return false;

    if (out.capacity() < out.size() + 8)
        out.reserve(out.size() + 8);

    std::size_t pos = 1;
    const std::size_t n = arr.size();
    bool first = true;
    while (pos < n)
    {
        while (pos < n && (arr[pos] == ' ' || arr[pos] == '\n'
                           || arr[pos] == '\r' || arr[pos] == '\t'))
            ++pos;
        if (pos >= n) return false;
        if (arr[pos] == ']') return pos + 1 == n;
        if (!first)
        {
            if (arr[pos] != ',') return false;
            ++pos;
            while (pos < n && (arr[pos] == ' ' || arr[pos] == '\n'
                               || arr[pos] == '\r' || arr[pos] == '\t'))
                ++pos;
            if (pos >= n || arr[pos] == ']') return false;
        }
        if (arr[pos] != '[') return false;
        ++pos;

        skip_ws(arr, pos);
        if (pos >= n || arr[pos] != '"') return false;
        ++pos;
        auto price_end = arr.find('"', pos);
        if (price_end == std::string_view::npos) return false;
        std::string_view price_sv = arr.substr(pos, price_end - pos);
        pos = price_end + 1;

        skip_ws(arr, pos);
        if (pos >= n || arr[pos] != ',') return false;
        ++pos;
        skip_ws(arr, pos);

        if (pos >= n || arr[pos] != '"') return false;
        ++pos;
        auto qty_end = arr.find('"', pos);
        if (qty_end == std::string_view::npos) return false;
        std::string_view qty_sv = arr.substr(pos, qty_end - pos);
        pos = qty_end + 1;

        skip_ws(arr, pos);
        if (pos >= n || arr[pos] != ']') return false;
        ++pos;

        double price = 0.0;
        if (!parse_double_sv(price_sv, price) || !(price > 0.0))
            return false;
        const auto qty_atoms =
            tt::quantity_scale::parse_decimal_canonical_atoms(qty_sv);
        // Bitget books* messages are snapshots. A zero quantity is a delta
        // delete semantic and is not valid in this parser.
        if (!qty_atoms || *qty_atoms <= 0)
            return false;
        out.push_back({price, *qty_atoms});
        first = false;
    }
    return false;
}

inline bool parse_book_side(
    std::string_view body, std::string_view short_key,
    std::string_view long_key,
    std::vector<provider::l2_snapshot::level>& out)
{
    std::string_view ignored;
    const auto short_result = provider_recovery::payload_parser(body)
        .inspect_top_level_member(short_key, ignored);
    const auto long_result = provider_recovery::payload_parser(body)
        .inspect_top_level_member(long_key, ignored);
    if (short_result
            == provider_recovery::payload_parser::member_result::invalid_or_duplicate
        || long_result
            == provider_recovery::payload_parser::member_result::invalid_or_duplicate
        || (short_result == provider_recovery::payload_parser::member_result::unique
            && long_result == provider_recovery::payload_parser::member_result::unique))
        return false;
    const auto key = short_result
            == provider_recovery::payload_parser::member_result::unique
        ? short_key : long_key;
    return append_levels(body, key, out);
}

inline std::optional<std::chrono::system_clock::time_point>
parse_ts_ms(std::string_view sv)
{
    int64_t ts_ms = 0;
    if (!parse_int64_sv(sv, ts_ms))
        return std::nullopt;
    return tt::date_parse::from_epoch_milliseconds(ts_ms);
}

enum class optional_time_result { missing, valid, invalid };

inline optional_time_result parse_optional_frame_time(
    std::string_view envelope,
    std::chrono::system_clock::time_point& out)
{
    std::string_view raw;
    const auto state = provider_recovery::payload_parser(envelope)
        .inspect_top_level_member("ts", raw);
    if (state == provider_recovery::payload_parser::member_result::missing)
        return optional_time_result::missing;
    if (state
            != provider_recovery::payload_parser::member_result::unique
        || !provider_recovery::top_level_scalar_text(envelope, "ts", raw))
        return optional_time_result::invalid;
    const auto parsed = parse_ts_ms(raw);
    if (!parsed) return optional_time_result::invalid;
    out = *parsed;
    return optional_time_result::valid;
}

// Map Bitget taker side → aggressor side (Binance semantics).
// buy → buyer aggressor → bid; sell → seller aggressor → ask.
inline std::optional<data_tick_side> map_side(std::string_view side)
{
    if (side == "buy" || side == "BUY" || side == "Buy")
        return data_tick_side::bid;
    if (side == "sell" || side == "SELL" || side == "Sell")
        return data_tick_side::ask;
    return std::nullopt;
}

inline uint8_t side_to_u8(data_tick_side s)
{
    switch (s)
    {
    case data_tick_side::bid: return 0;
    case data_tick_side::ask: return 1;
    default: return 2;
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public needle extractors
// ---------------------------------------------------------------------------

inline std::string_view extract_sv_string(std::string_view json, std::string_view key)
{
    auto colon = detail::find_key(json, key);
    if (colon == std::string_view::npos) return {};
    std::size_t pos = colon + 1;
    detail::skip_ws(json, pos);
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

inline std::string extract_string(const std::string& json, const std::string& key)
{
    return std::string(extract_sv_string(json, key));
}

inline std::string extract_number(const std::string& json, const std::string& key)
{
    return std::string(extract_sv_number(json, key));
}

inline bool extract_bool(const std::string& json, const std::string& key)
{
    return extract_sv_bool(json, key);
}

inline bool parse_double_sv(std::string_view sv, double& out)
{
    return detail::parse_double_sv(sv, out);
}

inline bool parse_int64_sv(std::string_view sv, int64_t& out)
{
    return detail::parse_int64_sv(sv, out);
}

inline bool parse_nonzero_uint64_sv(std::string_view sv,
                                    std::uint64_t& out)
{
    sv = provider_recovery::trim_json_ws(sv);
    if (sv.empty()) return false;
    for (const char c : sv)
        if (c < '0' || c > '9') return false;
    const auto [end, error] =
        std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return error == std::errc{} && end == sv.data() + sv.size()
        && out != 0;
}

// ---------------------------------------------------------------------------
// Trade
// ---------------------------------------------------------------------------

// Parse one trade object (fields i/p/v/S/T). Symbol comes from arg.symbol.
inline std::optional<provider::tick> parse_trade_object(std::string_view obj,
                                                        std::string_view symbol)
{
    if (symbol.empty() || !provider_recovery::is_authoritative_object(obj)
        || !provider_recovery::decision_members_are_unique(
            obj, {"i", "p", "v", "S", "side", "T", "ts"})
        || !detail::at_most_one_top_level_member(obj, {"S", "side"})
        || !detail::at_most_one_top_level_member(obj, {"T", "ts"}))
        return std::nullopt;

    std::string_view price_sv;
    std::string_view qty_sv;
    std::string_view side_sv;
    std::string_view time_sv;
    std::string_view native_id_sv;

    detail::for_each_flat_field(obj, 0, obj.size(),
        [&](std::string_view key, std::string_view value) {
            // Prefer short wire keys; accept long aliases without key_tag
            // (unsigned 32-bit tag overflows past 4 chars).
            if (key == "i") native_id_sv = value;
            else if (key == "p") price_sv = value;
            else if (key == "v") qty_sv = value;
            else if (key == "S") side_sv = value;
            else if (key == "side" && side_sv.empty()) side_sv = value;
            else if (key == "T") time_sv = value;
            else if (key == "ts" && time_sv.empty()) time_sv = value;
        });

    if (native_id_sv.empty() || price_sv.empty() || qty_sv.empty()
        || side_sv.empty())
        return std::nullopt;

    std::uint64_t native_id = 0;
    if (!parse_nonzero_uint64_sv(native_id_sv, native_id))
        return std::nullopt;

    double price = 0.0;
    if (!detail::parse_double_sv(price_sv, price) || !(price > 0.0))
        return std::nullopt;
    const auto qty_atoms =
        tt::quantity_scale::parse_decimal_canonical_atoms(qty_sv);
    if (!qty_atoms || *qty_atoms <= 0)
        return std::nullopt;

    auto side = detail::map_side(side_sv);
    if (!side) return std::nullopt;

    provider::tick t;
    t.symbol.assign(symbol.data(), symbol.size());
    t.price = price;
    t.quantity = *qty_atoms;
    t.quantity_scale = tt::quantity_scale::canonical_atoms;
    t.side = detail::side_to_u8(*side);
    t.native_trade_id = native_id;

    const auto tp = detail::parse_ts_ms(time_sv);
    if (!tp) return std::nullopt;
    t.timestamp = *tp;

    return t;
}

// Full UTA publicTrade WS push → **first** trade in data[] only.
// Multi-trade frames: use parse_all_trades() / BitgetTradeParser::parse_records.
inline std::optional<provider::tick> parse_trade(std::string_view json)
{
    if (!detail::authoritative_public_envelope(json))
        return std::nullopt;
    auto arg = detail::top_level_object(json, "arg");
    if (!detail::authoritative_arg(arg)
        || !detail::authoritative_usdt_public_trade(json, arg))
        return std::nullopt;
    std::string_view symbol;
    std::string_view topic;
    if (!arg.empty())
    {
        (void)provider_recovery::top_level_plain_string(arg, "topic", topic);
    }
    if (!detail::resolve_public_symbol(json, arg, symbol))
        return std::nullopt;

    if (topic != "publicTrade") return std::nullopt;

    std::chrono::system_clock::time_point frame_time;
    const auto frame_time_state = detail::parse_optional_frame_time(
        json, frame_time);
    if (frame_time_state != detail::optional_time_result::valid)
        return std::nullopt;

    auto arr = detail::top_level_array(json, "data");
    if (arr.empty())
    {
        auto parsed = parse_trade_object(json, symbol);
        if (parsed && parsed->timestamp > frame_time)
            return std::nullopt;
        if (parsed) parsed->timestamp = frame_time;
        return parsed;
    }

    std::optional<provider::tick> first;
    bool invalid_element = false;
    const bool valid_array = detail::for_each_array_object(
        arr, [&](std::string_view obj) {
            auto parsed = parse_trade_object(obj, symbol);
            if (!parsed)
            {
                invalid_element = true;
                return;
            }
            if (parsed->timestamp > frame_time)
            {
                invalid_element = true;
                return;
            }
            parsed->timestamp = frame_time;
            if (!first)
                first = std::move(parsed);
        });
    if (!valid_array || invalid_element)
        return std::nullopt;
    return first;
}

// All trades in a publicTrade push (data[]). Provider-facing batch API:
// when data[] has N trades, returns N ticks (empty vector on miss/malformed).
// Production path: BitgetTradeParser::parse_records → DataBridge multi-emit.
inline std::vector<provider::tick> parse_all_trades(std::string_view json)
{
    std::vector<provider::tick> out;
    if (!detail::authoritative_public_envelope(json))
        return out;
    auto arg = detail::top_level_object(json, "arg");
    if (!detail::authoritative_arg(arg)
        || !detail::authoritative_usdt_public_trade(json, arg))
        return out;
    std::string_view symbol;
    std::string_view topic;
    if (!arg.empty())
    {
        (void)provider_recovery::top_level_plain_string(arg, "topic", topic);
    }
    if (!detail::resolve_public_symbol(json, arg, symbol))
        return out;
    if (topic != "publicTrade") return out;

    std::chrono::system_clock::time_point frame_time;
    const auto frame_time_state = detail::parse_optional_frame_time(
        json, frame_time);
    if (frame_time_state != detail::optional_time_result::valid)
        return out;

    auto arr = detail::top_level_array(json, "data");
    if (arr.empty()) return out;

    bool invalid_element = false;
    std::optional<std::chrono::system_clock::time_point> last_time;
    const bool valid_array = detail::for_each_array_object(
        arr, [&](std::string_view obj) {
        if (auto t = parse_trade_object(obj, symbol))
        {
            const auto occurrence_time = t->timestamp;
            if ((last_time && occurrence_time < *last_time)
                || occurrence_time > frame_time)
            {
                invalid_element = true;
                return;
            }
            for (const auto& existing : out) {
                if (existing.native_trade_id == t->native_trade_id) {
                    invalid_element = true;
                    return;
                }
            }
            last_time = occurrence_time;
            // provider::tick currently exposes one engine time. Use the
            // authoritative envelope publication time so strategies cannot
            // observe the trade before the frame was knowable.
            t->timestamp = frame_time;
            out.push_back(std::move(*t));
        }
        else
            invalid_element = true;
    });
    if (!valid_array || invalid_element)
        out.clear();
    return out;
}

inline tick_record tick_to_record(const provider::tick& t)
{
    tick_record rec;
    rec.timestamp = t.timestamp;
    rec.symbol    = t.symbol;
    rec.price     = t.price;
    rec.quantity  = t.quantity;
    rec.side      = static_cast<data_tick_side>(t.side);
    rec.quantity_scale = t.quantity_scale;
    return rec;
}

inline std::optional<tick_record> parse_trade_record(std::string_view json)
{
    auto t = parse_trade(json);
    if (!t) return std::nullopt;
    return tick_to_record(*t);
}

// All publicTrade elements as tick_record (production multi-emit path).
inline std::vector<tick_record> parse_all_trade_records(std::string_view json)
{
    std::vector<tick_record> out;
    auto ticks = parse_all_trades(json);
    out.reserve(ticks.size());
    for (const auto& t : ticks)
        out.push_back(tick_to_record(t));
    // Flat / single-object frames without data[] array.
    if (out.empty())
    {
        if (auto one = parse_trade_record(json))
            out.push_back(std::move(*one));
    }
    return out;
}

// ---------------------------------------------------------------------------
// books5 → l2_snapshot
// ---------------------------------------------------------------------------

inline std::optional<provider::l2_snapshot> parse_books5(std::string_view json)
{
    if (!detail::authoritative_public_envelope(json))
        return std::nullopt;
    auto arg = detail::top_level_object(json, "arg");
    if (!detail::authoritative_arg(arg))
        return std::nullopt;
    std::string_view symbol;
    std::string_view topic;
    if (!arg.empty())
    {
        (void)provider_recovery::top_level_plain_string(arg, "topic", topic);
    }
    if (!detail::resolve_public_symbol(json, arg, symbol))
        return std::nullopt;

    // Action / topic gates (Phase 0 snapshot path only):
    // - books5 / books1 / books50: always snapshot; missing action OK;
    //   reject action=update (or any non-snapshot).
    // - books (full): require action=="snapshot"; deltas rejected.
    std::string_view action;
    (void)provider_recovery::top_level_plain_string(json, "action", action);
    if (!topic.empty())
    {
        const bool limited =
            topic == "books5" || topic == "books1" || topic == "books50";
        const bool full = topic == "books";
        if (!limited && !full)
            return std::nullopt;
        if (full)
        {
            if (action != "snapshot")
                return std::nullopt;
        }
        else if (!action.empty() && action != "snapshot")
        {
            return std::nullopt;
        }
    }

    auto obj = detail::first_data_object(json);
    std::string_view body = obj.empty() ? json : obj;
    if (!provider_recovery::is_authoritative_object(body)
        || !provider_recovery::decision_members_are_unique(
            body, {"a", "asks", "b", "bids", "ts"}))
        return std::nullopt;

    provider::l2_snapshot snap;
    snap.symbol.assign(symbol.data(), symbol.size());
    snap.quantity_scale = 100'000'000ULL;

    std::chrono::system_clock::time_point frame_time;
    if (detail::parse_optional_frame_time(json, frame_time)
        != detail::optional_time_result::valid)
        return std::nullopt;
    std::string_view body_ts_sv;
    if (!provider_recovery::top_level_scalar_text(body, "ts", body_ts_sv))
        return std::nullopt;
    const auto body_time = detail::parse_ts_ms(body_ts_sv);
    if (!body_time || *body_time > frame_time) return std::nullopt;
    snap.timestamp = frame_time;

    bool levels_valid = detail::parse_book_side(
        body, "b", "bids", snap.bids);
    if (levels_valid)
        levels_valid = detail::parse_book_side(
            body, "a", "asks", snap.asks);
    if (!levels_valid)
        return std::nullopt;

    if (snap.bids.empty() && snap.asks.empty())
        return std::nullopt;

    return snap;
}

// ---------------------------------------------------------------------------
// kline → bar
// ---------------------------------------------------------------------------

inline std::optional<provider::bar> parse_kline(std::string_view json)
{
    if (!detail::authoritative_public_envelope(json))
        return std::nullopt;
    auto arg = detail::top_level_object(json, "arg");
    if (!detail::authoritative_arg(arg))
        return std::nullopt;
    std::string_view symbol;
    std::string_view topic;
    if (!arg.empty())
    {
        (void)provider_recovery::top_level_plain_string(arg, "topic", topic);
    }
    if (!detail::resolve_public_symbol(json, arg, symbol))
        return std::nullopt;

    if (!topic.empty() && topic != "kline")
        return std::nullopt;

    auto obj = detail::first_data_object(json);
    std::string_view body = obj.empty() ? json : obj;
    if (!provider_recovery::is_authoritative_object(body)
        || !provider_recovery::decision_members_are_unique(
            body, {"open", "high", "low", "close", "volume", "vol",
                   "start", "o", "h", "l", "c", "v", "t", "confirm"})
        || !detail::at_most_one_top_level_member(body, {"open", "o"})
        || !detail::at_most_one_top_level_member(body, {"high", "h"})
        || !detail::at_most_one_top_level_member(body, {"low", "l"})
        || !detail::at_most_one_top_level_member(body, {"close", "c"})
        || !detail::at_most_one_top_level_member(
            body, {"volume", "vol", "v"})
        || !detail::at_most_one_top_level_member(body, {"start", "t"}))
        return std::nullopt;
    std::string_view confirm_raw;
    const auto confirm_state = provider_recovery::payload_parser(body)
        .inspect_top_level_member("confirm", confirm_raw);
    if (confirm_state
        == provider_recovery::payload_parser::member_result::unique)
    {
        confirm_raw = provider_recovery::trim_json_ws(confirm_raw);
        if (confirm_raw != "true" && confirm_raw != "false")
            return std::nullopt;
    }

    // Pure OHLCV parse — no closed-bar policy here. UTA kline pushes have no
    // `confirm` field and update the open candle ~1/s; closed-bar emission is
    // characterized by kline_closed_gate. Production adapters currently
    // refuse every Bitget candle until the frozen engine contract can carry
    // candle-open and causal known/decision timestamps separately.

    std::string_view open_sv, high_sv, low_sv, close_sv, vol_sv, start_sv;

    detail::for_each_flat_field(body, 0, body.size(),
        [&](std::string_view key, std::string_view value) {
            // Long keys compared by string_view (key_tag is 32-bit / ≤4 chars).
            if (key == "open") open_sv = value;
            else if (key == "high") high_sv = value;
            else if (key == "low") low_sv = value;
            else if (key == "close") close_sv = value;
            else if (key == "volume") vol_sv = value;
            else if (key == "vol" && vol_sv.empty()) vol_sv = value;
            else if (key == "start") start_sv = value;
            else if (key == "o" && open_sv.empty()) open_sv = value;
            else if (key == "h" && high_sv.empty()) high_sv = value;
            else if (key == "l" && low_sv.empty()) low_sv = value;
            else if (key == "c" && close_sv.empty()) close_sv = value;
            else if (key == "v" && vol_sv.empty()) vol_sv = value;
            else if (key == "t" && start_sv.empty()) start_sv = value;
        });

    if (open_sv.empty() || high_sv.empty() || low_sv.empty() || close_sv.empty()
        || vol_sv.empty() || !detail::parse_ts_ms(start_sv))
        return std::nullopt;

    provider::bar b;
    b.symbol.assign(symbol.data(), symbol.size());
    b.quantity_scale = 100'000'000ULL;

    double v = 0.0;
    if (!detail::parse_double_sv(open_sv, v) || !(v > 0.0)) return std::nullopt;
    b.open = v;
    if (!detail::parse_double_sv(high_sv, v) || !(v > 0.0)) return std::nullopt;
    b.high = v;
    if (!detail::parse_double_sv(low_sv, v) || !(v > 0.0)) return std::nullopt;
    b.low = v;
    if (!detail::parse_double_sv(close_sv, v) || !(v > 0.0)) return std::nullopt;
    b.close = v;

    if (b.high < std::max(b.open, b.close)
        || b.low > std::min(b.open, b.close)
        || b.high < b.low)
        return std::nullopt;

    const auto volume_atoms =
        tt::quantity_scale::parse_decimal_canonical_atoms(vol_sv);
    // Zero is an unlimited-liquidity sentinel in the current bar execution
    // model, so an external venue bar must carry positive exact volume.
    if (!volume_atoms || *volume_atoms <= 0)
        return std::nullopt;
    b.volume = *volume_atoms;

    b.date.assign(start_sv.data(), start_sv.size());

    if (confirm_state
            == provider_recovery::payload_parser::member_result::unique
        && provider_recovery::trim_json_ws(confirm_raw) == "true")
    {
        std::string_view known_text;
        std::int64_t start_ms = 0;
        std::int64_t known_ms = 0;
        if (!provider_recovery::top_level_scalar_text(
                json, "ts", known_text)
            || !detail::parse_int64_sv(start_sv, start_ms)
            || !detail::parse_int64_sv(known_text, known_ms)
            || !detail::parse_ts_ms(known_text)
            || known_ms < start_ms)
            return std::nullopt;
    }

    return b;
}

// Optional confirm flag on kline body (classic/legacy). UTA has no confirm.
inline std::optional<bool> extract_kline_confirm(std::string_view json)
{
    auto obj = detail::first_data_object(json);
    std::string_view body = obj.empty() ? json : obj;
    std::string_view raw;
    if (provider_recovery::top_level_member(body, "confirm", raw))
    {
        raw = provider_recovery::trim_json_ws(raw);
        if (raw == "true") return true;
        if (raw == "false") return false;
        return std::nullopt;
    }
    if (provider_recovery::top_level_member(json, "confirm", raw))
    {
        raw = provider_recovery::trim_json_ws(raw);
        if (raw == "true") return true;
        if (raw == "false") return false;
    }
    return std::nullopt;
}

// Closed-bar policy for Bitget klines:
//   - confirm:true  → emit immediately (legacy closed candle)
//   - confirm:false → buffer open candle, do not emit
//   - confirm absent (UTA) → buffer; emit previous bar when `date`/`start`
//     advances (start rollover). First open candle is held until the next
//     period starts — avoids treating mid-candle updates as completed bars.
struct kline_closed_gate
{
    std::optional<provider::bar> on_bar(provider::bar b,
                                        std::optional<bool> confirm = std::nullopt,
                                        std::optional<std::int64_t> known_ms = std::nullopt)
    {
        last_rejected_ = false;
        std::int64_t start_ms = 0;
        if (b.symbol.empty() || !detail::parse_int64_sv(b.date, start_ms)
            || start_ms <= 0)
        {
            last_rejected_ = true;
            return std::nullopt;
        }

        if (last_emitted_start_ms_)
        {
            if (b.symbol != last_emitted_symbol_
                || start_ms <= *last_emitted_start_ms_)
            {
                last_rejected_ = true;
                return std::nullopt;
            }
        }

        std::int64_t pending_start_ms = 0;
        if (pending_)
        {
            if (pending_->symbol != b.symbol
                || !detail::parse_int64_sv(
                    pending_->date, pending_start_ms)
                || pending_start_ms <= 0 || start_ms < pending_start_ms)
            {
                last_rejected_ = true;
                return std::nullopt;
            }
        }

        if (confirm.has_value())
        {
            if (!*confirm)
            {
                if (pending_ && start_ms > pending_start_ms)
                {
                    auto closed = std::move(*pending_);
                    pending_ = std::move(b);
                    last_emitted_symbol_ = closed.symbol;
                    last_emitted_start_ms_ = pending_start_ms;
                    closed.date = std::to_string(start_ms);
                    return closed;
                }
                pending_ = std::move(b);
                return std::nullopt;
            }
            // A pending candle may only be finalized by the same symbol/start.
            // Skipping across starts would silently discard an observation.
            if (!known_ms || *known_ms < start_ms
                || (pending_ && start_ms != pending_start_ms))
            {
                last_rejected_ = true;
                return std::nullopt;
            }
            pending_.reset();
            last_emitted_symbol_ = b.symbol;
            last_emitted_start_ms_ = start_ms;
            b.date = std::to_string(*known_ms);
            return b;
        }

        if (!pending_)
        {
            pending_ = std::move(b);
            return std::nullopt;
        }
        if (start_ms == pending_start_ms)
        {
            pending_ = std::move(b); // in-progress update
            return std::nullopt;
        }
        auto closed = std::move(*pending_);
        pending_ = std::move(b);
        last_emitted_symbol_ = closed.symbol;
        last_emitted_start_ms_ = pending_start_ms;
        // The previous candle only becomes knowable at this rollover. Carry
        // causal decision time, not the old candle-open timestamp.
        closed.date = std::to_string(start_ms);
        return closed;
    }

    void reset()
    {
        pending_.reset();
        last_emitted_symbol_.clear();
        last_emitted_start_ms_.reset();
        last_rejected_ = false;
    }

    const std::optional<provider::bar>& pending() const { return pending_; }
    bool last_rejected() const noexcept { return last_rejected_; }

private:
    std::optional<provider::bar> pending_;
    std::string last_emitted_symbol_;
    std::optional<std::int64_t> last_emitted_start_ms_;
    bool last_rejected_ = false;
};

inline std::optional<bar_record> to_bar_record(const provider::bar& b)
{
    bar_record rec;
    rec.date   = b.date;
    rec.symbol = b.symbol;
    rec.open   = b.open;
    rec.high   = b.high;
    rec.low    = b.low;
    rec.close  = b.close;
    rec.volume = b.volume;
    rec.quantity_scale = b.quantity_scale;
    return rec;
}

inline std::optional<bar_record> parse_kline_record(std::string_view json)
{
    auto b = parse_kline(json);
    if (!b) return std::nullopt;
    // Qualify: provider::to_bar_record also exists via provider_convert.h
    // when this header is pulled into main.inc with ENABLE_BITGET.
    return bitget::to_bar_record(*b);
}

// ---------------------------------------------------------------------------
// Combined dispatcher (arg.topic → event)
// ---------------------------------------------------------------------------
// Single-event surface for IDataParser<provider::event>. For publicTrade,
// parse_ws_message returns the first trade only; BitgetCombinedParser
// overrides parse_records to emit the full data[] batch.
//
// Kline: raw parse only for validation/tests. Production adapters currently
// fail closed on Bitget candles because the engine has no dual-time contract.

inline std::optional<provider::event> parse_ws_message(std::string_view json)
{
    auto arg = detail::top_level_object(json, "arg");
    std::string_view topic;
    if (!arg.empty())
        (void)provider_recovery::top_level_plain_string(arg, "topic", topic);

    if (topic == "publicTrade")
    {
        // First trade only — see parse_all_trades for full data[] batch.
        auto t = parse_trade(json);
        if (!t) return std::nullopt;
        return provider::event{std::move(*t)};
    }
    if (topic == "books5" || topic == "books1" || topic == "books50" || topic == "books")
    {
        auto snap = parse_books5(json);
        if (!snap) return std::nullopt;
        return provider::event{std::move(*snap)};
    }
    if (topic == "kline")
    {
        auto b = parse_kline(json);
        if (!b) return std::nullopt;
        return provider::event{std::move(*b)};
    }

    if (topic.empty())
    {
        if (auto t = parse_trade(json))
            return provider::event{std::move(*t)};
        if (auto snap = parse_books5(json))
            return provider::event{std::move(*snap)};
        if (auto b = parse_kline(json))
            return provider::event{std::move(*b)};
    }

    return std::nullopt;
}

// Apply closed-bar gate to a kline frame. Returns closed bar when ready.
inline std::optional<provider::bar>
gated_kline_bar(kline_closed_gate& gate, std::string_view json)
{
    auto b = parse_kline(json);
    if (!b) return std::nullopt;
    const auto confirm = extract_kline_confirm(json);
    std::optional<std::int64_t> known_ms;
    if (confirm && *confirm)
    {
        std::string_view text;
        std::int64_t parsed = 0;
        if (!provider_recovery::top_level_scalar_text(json, "ts", text)
            || !detail::parse_int64_sv(text, parsed)
            || !detail::parse_ts_ms(text))
            return std::nullopt;
        known_ms = parsed;
    }
    return gate.on_bar(std::move(*b), confirm, known_ms);
}

} // namespace bitget

// IDataParser adapters (outside namespace, matching Binance style).

// Emits every publicTrade data[] element via parse_records (DataBridge).
class BitgetTradeParser : public IDataParser<tick_record>
{
public:
    bool parse_header(const std::string&) override { return true; }

    std::optional<tick_record> parse_record(const std::string& line) override
    {
        return parse_record(std::string_view{line});
    }

    std::optional<tick_record> parse_record(std::string_view line) override
    {
        auto batch = parse_records(line);
        if (batch.empty()) return std::nullopt;
        return std::move(batch.front());
    }

    std::vector<tick_record> parse_records(std::string_view line) override
    {
        auto batch = bitget::parse_all_trade_records(line);
        if (batch.empty()) return batch;
        if (last_timestamp_ && batch.front().timestamp < *last_timestamp_)
            return {};
        last_timestamp_ = batch.back().timestamp;
        return batch;
    }

private:
    std::optional<std::chrono::system_clock::time_point> last_timestamp_;
};

// Stateful: only emits closed bars (start rollover / confirm:true).
class BitgetKlineParser : public IDataParser<bar_record>
{
public:
    bool parse_header(const std::string&) override { return true; }

    std::optional<bar_record> parse_record(const std::string& line) override
    {
        return parse_record(std::string_view{line});
    }

    std::optional<bar_record> parse_record(std::string_view line) override
    {
        // The frozen engine contract has only one bar timestamp and treats it
        // as candle open in batch while streaming needs a distinct known-at /
        // decision time. Until that contract carries both clocks, emitting a
        // Bitget candle would make batch/stream decisions diverge.
        (void)line;
        return std::nullopt;
    }

private:
    [[maybe_unused]]
    bitget::kline_closed_gate gate_;
};

class BitgetBooksParser : public IDataParser<provider::l2_snapshot>
{
public:
    bool parse_header(const std::string&) override { return true; }

    std::optional<provider::l2_snapshot> parse_record(const std::string& line) override
    {
        return bitget::parse_books5(std::string_view{line});
    }

    std::optional<provider::l2_snapshot> parse_record(std::string_view line) override
    {
        return bitget::parse_books5(line);
    }
};

// Combined event adapter. publicTrade → full data[] via parse_records;
// kline path is explicitly unsupported/fail-closed (same as BitgetKlineParser).
class BitgetCombinedParser : public IDataParser<provider::event>
{
public:
    bool parse_header(const std::string&) override { return true; }

    std::optional<provider::event> parse_record(const std::string& line) override
    {
        return parse_record(std::string_view{line});
    }

    std::optional<provider::event> parse_record(std::string_view line) override
    {
        auto batch = parse_records(line);
        if (batch.empty()) return std::nullopt;
        return std::move(batch.front());
    }

    std::vector<provider::event> parse_records(std::string_view line) override
    {
        std::vector<provider::event> out;
        last_kline_rejected_ = false;
        auto arg = bitget::detail::top_level_object(line, "arg");
        std::string_view topic;
        if (!arg.empty())
            (void)provider_recovery::top_level_plain_string(
                arg, "topic", topic);

        if (topic == "publicTrade" || topic.empty())
        {
            auto ticks = bitget::parse_all_trades(line);
            if (!ticks.empty())
            {
                if (last_timestamp_
                    && ticks.front().timestamp < *last_timestamp_)
                    return out;
                last_timestamp_ = ticks.back().timestamp;
                out.reserve(ticks.size());
                for (auto& t : ticks)
                    out.emplace_back(std::move(t));
                return out;
            }
        }

        if (topic == "kline")
        {
            // Explicitly unsupported until the frozen engine model exposes a
            // separate causal decision timestamp for completed candles.
            last_kline_rejected_ = true;
            return out;
        }

        // Empty topic: if frame parses as kline, apply gate (do not fall
        // through to raw parse_ws_message which would emit open candles).
        if (topic.empty())
        {
            if (bitget::parse_kline(line))
            {
                last_kline_rejected_ = true;
                return out;
            }
        }

        if (auto ev = bitget::parse_ws_message(line))
            out.push_back(std::move(*ev));
        return out;
    }

    empty_parse_status classify_empty_frame(std::string_view line) const override
    {
        if (last_kline_rejected_)
            return empty_parse_status::malformed;
        if (!provider_recovery::is_authoritative_object(line))
            return empty_parse_status::malformed;
        std::string_view arg;
        std::string_view topic;
        const bool has_arg = provider_recovery::top_level_member(
                                 line, "arg", arg)
            && provider_recovery::is_authoritative_object(arg);
        if (has_arg)
            (void)provider_recovery::top_level_plain_string(
                arg, "topic", topic);
        if ((topic == "kline" || topic.empty()) && bitget::parse_kline(line))
            return empty_parse_status::ignored;
        std::string_view data;
        const bool no_data = provider_recovery::payload_parser(line)
            .inspect_top_level_member("data", data)
            == provider_recovery::payload_parser::member_result::missing;
        std::string_view event;
        if (!provider_recovery::top_level_plain_string(
                line, "event", event))
            return empty_parse_status::malformed;
        if (no_data && event == "subscribe" && has_arg && !topic.empty())
        {
            std::string_view code_raw;
            const auto code_state = provider_recovery::payload_parser(line)
                .inspect_top_level_member("code", code_raw);
            if (code_state
                == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
                return empty_parse_status::malformed;
            if (code_state
                == provider_recovery::payload_parser::member_result::unique)
            {
                std::string_view code;
                if (!provider_recovery::top_level_scalar_text(
                        line, "code", code)
                    || (!code.empty() && code != "0" && code != "00000"))
                    return empty_parse_status::malformed;
            }
            return empty_parse_status::ignored;
        }
        if (no_data && event == "pong")
            return empty_parse_status::ignored;
        return empty_parse_status::malformed;
    }

private:
    bitget::kline_closed_gate kline_gate_;
    std::optional<std::chrono::system_clock::time_point> last_timestamp_;
    bool last_kline_rejected_ = false;
};

#endif // HAS_BITGET
