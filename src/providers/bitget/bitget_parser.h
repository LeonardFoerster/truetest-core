#pragma once
#ifdef HAS_BITGET

#include "data/data_handler.h"
#include "providers/local/csv_parser.h"
#include "providers/parser.h"
#include "providers/provider_event.h"

#include <charconv>
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
    return ec == std::errc();
}

inline bool parse_int64_sv(std::string_view sv, int64_t& out)
{
    if (sv.empty()) return false;
    auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc();
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
        auto close = match_container(array, pos);
        if (close == std::string_view::npos) break;
        fn(array.substr(pos, close - pos + 1));
        pos = close + 1;
    }
}

inline std::string_view first_data_object(std::string_view json)
{
    auto arr = extract_array(json, "data");
    if (arr.empty()) return {};
    std::string_view first;
    for_each_array_object(arr, [&](std::string_view obj) {
        if (first.empty()) first = obj;
    });
    return first;
}

// Locate `"key":[` and append levels into out. Qty scaled *1e8 like Binance.
inline void append_levels(std::string_view json, std::string_view key,
                          std::vector<provider::l2_snapshot::level>& out)
{
    auto arr = extract_array(json, key);
    if (arr.size() < 2) return;

    if (out.capacity() < out.size() + 8)
        out.reserve(out.size() + 8);

    std::size_t pos = 1;
    const std::size_t n = arr.size();
    while (pos < n)
    {
        while (pos < n && (arr[pos] == ' ' || arr[pos] == ',' || arr[pos] == '\n' ||
                           arr[pos] == '\r' || arr[pos] == '\t'))
            ++pos;
        if (pos >= n || arr[pos] == ']') break;
        if (arr[pos] != '[') break;
        ++pos;

        skip_ws(arr, pos);
        if (pos >= n || arr[pos] != '"') break;
        ++pos;
        auto price_end = arr.find('"', pos);
        if (price_end == std::string_view::npos) break;
        std::string_view price_sv = arr.substr(pos, price_end - pos);
        pos = price_end + 1;

        while (pos < n && (arr[pos] == ',' || arr[pos] == ' ' || arr[pos] == '\t'))
            ++pos;

        if (pos >= n || arr[pos] != '"') break;
        ++pos;
        auto qty_end = arr.find('"', pos);
        if (qty_end == std::string_view::npos) break;
        std::string_view qty_sv = arr.substr(pos, qty_end - pos);
        pos = qty_end + 1;

        while (pos < n && arr[pos] != ']') ++pos;
        if (pos < n) ++pos;

        double price = 0.0, qty = 0.0;
        if (!parse_double_sv(price_sv, price) || !parse_double_sv(qty_sv, qty))
            break;
        out.push_back({price, static_cast<int64_t>(qty * 1e8)});
    }
}

inline std::chrono::system_clock::time_point tp_from_ms(int64_t ts_ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ts_ms));
}

inline std::optional<std::chrono::system_clock::time_point>
parse_ts_ms(std::string_view sv)
{
    int64_t ts_ms = 0;
    if (!parse_int64_sv(sv, ts_ms)) return std::nullopt;
    return tp_from_ms(ts_ms);
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

// ---------------------------------------------------------------------------
// Trade
// ---------------------------------------------------------------------------

// Parse one trade object (fields i/p/v/S/T). Symbol comes from arg.symbol.
inline std::optional<provider::tick> parse_trade_object(std::string_view obj,
                                                        std::string_view symbol)
{
    if (symbol.empty()) return std::nullopt;

    std::string_view price_sv;
    std::string_view qty_sv;
    std::string_view side_sv;
    std::string_view time_sv;

    detail::for_each_flat_field(obj, 0, obj.size(),
        [&](std::string_view key, std::string_view value) {
            // Prefer short wire keys; accept long aliases without key_tag
            // (unsigned 32-bit tag overflows past 4 chars).
            if (key == "p") price_sv = value;
            else if (key == "v") qty_sv = value;
            else if (key == "S") side_sv = value;
            else if (key == "side" && side_sv.empty()) side_sv = value;
            else if (key == "T") time_sv = value;
            else if (key == "ts" && time_sv.empty()) time_sv = value;
        });

    if (price_sv.empty() || qty_sv.empty() || side_sv.empty())
        return std::nullopt;

    double price = 0.0, qty = 0.0;
    if (!detail::parse_double_sv(price_sv, price)) return std::nullopt;
    if (!detail::parse_double_sv(qty_sv, qty)) return std::nullopt;

    auto side = detail::map_side(side_sv);
    if (!side) return std::nullopt;

    provider::tick t;
    t.symbol.assign(symbol.data(), symbol.size());
    t.price = price;
    t.quantity = static_cast<int64_t>(qty * 1e8);
    t.side = detail::side_to_u8(*side);

    if (auto tp = detail::parse_ts_ms(time_sv))
        t.timestamp = *tp;
    else
        t.timestamp = std::chrono::system_clock::now();

    return t;
}

// Full UTA publicTrade WS push → **first** trade in data[] only.
// Multi-trade frames: use parse_all_trades() / BitgetTradeParser::parse_records.
inline std::optional<provider::tick> parse_trade(std::string_view json)
{
    auto arg = detail::extract_object(json, "arg");
    std::string_view symbol;
    std::string_view topic;
    if (!arg.empty())
    {
        symbol = extract_sv_string(arg, "symbol");
        topic  = extract_sv_string(arg, "topic");
    }
    if (symbol.empty())
        symbol = extract_sv_string(json, "symbol");

    if (!topic.empty() && topic != "publicTrade")
        return std::nullopt;

    auto obj = detail::first_data_object(json);
    if (obj.empty())
        return parse_trade_object(json, symbol);
    return parse_trade_object(obj, symbol);
}

// All trades in a publicTrade push (data[]). Provider-facing batch API:
// when data[] has N trades, returns N ticks (empty vector on miss/malformed).
// Production path: BitgetTradeParser::parse_records → DataBridge multi-emit.
inline std::vector<provider::tick> parse_all_trades(std::string_view json)
{
    std::vector<provider::tick> out;
    auto arg = detail::extract_object(json, "arg");
    std::string_view symbol;
    std::string_view topic;
    if (!arg.empty())
    {
        symbol = extract_sv_string(arg, "symbol");
        topic  = extract_sv_string(arg, "topic");
    }
    if (symbol.empty())
        symbol = extract_sv_string(json, "symbol");
    if (symbol.empty())
        return out;
    if (!topic.empty() && topic != "publicTrade")
        return out;

    auto arr = detail::extract_array(json, "data");
    if (arr.empty()) return out;

    detail::for_each_array_object(arr, [&](std::string_view obj) {
        if (auto t = parse_trade_object(obj, symbol))
            out.push_back(std::move(*t));
    });
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
    auto arg = detail::extract_object(json, "arg");
    std::string_view symbol;
    std::string_view topic;
    if (!arg.empty())
    {
        symbol = extract_sv_string(arg, "symbol");
        topic  = extract_sv_string(arg, "topic");
    }
    if (symbol.empty())
        symbol = extract_sv_string(json, "symbol");
    // Require symbol (same fail-closed rule as trade).
    if (symbol.empty())
        return std::nullopt;

    // Action / topic gates (Phase 0 snapshot path only):
    // - books5 / books1 / books50: always snapshot; missing action OK;
    //   reject action=update (or any non-snapshot).
    // - books (full): require action=="snapshot"; deltas rejected.
    auto action = extract_sv_string(json, "action");
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

    provider::l2_snapshot snap;
    snap.symbol.assign(symbol.data(), symbol.size());

    auto ts_sv = extract_sv_number(body, "ts");
    if (ts_sv.empty())
        ts_sv = extract_sv_number(json, "ts");
    if (auto tp = detail::parse_ts_ms(ts_sv))
        snap.timestamp = *tp;
    else
        snap.timestamp = std::chrono::system_clock::now();

    detail::append_levels(body, "b", snap.bids);
    if (snap.bids.empty())
        detail::append_levels(body, "bids", snap.bids);
    detail::append_levels(body, "a", snap.asks);
    if (snap.asks.empty())
        detail::append_levels(body, "asks", snap.asks);

    if (snap.bids.empty() && snap.asks.empty())
        return std::nullopt;

    return snap;
}

// ---------------------------------------------------------------------------
// kline → bar
// ---------------------------------------------------------------------------

inline std::optional<provider::bar> parse_kline(std::string_view json)
{
    auto arg = detail::extract_object(json, "arg");
    std::string_view symbol;
    std::string_view topic;
    std::string_view interval;
    if (!arg.empty())
    {
        symbol   = extract_sv_string(arg, "symbol");
        topic    = extract_sv_string(arg, "topic");
        interval = extract_sv_string(arg, "interval");
    }
    if (symbol.empty())
        symbol = extract_sv_string(json, "symbol");
    // Require symbol (same fail-closed rule as trade).
    if (symbol.empty())
        return std::nullopt;

    if (!topic.empty() && topic != "kline")
        return std::nullopt;

    auto obj = detail::first_data_object(json);
    std::string_view body = obj.empty() ? json : obj;

    // Pure OHLCV parse — no closed-bar policy here. UTA kline pushes have no
    // `confirm` field and update the open candle ~1/s; closed-bar emission is
    // handled by kline_closed_gate (start rollover) on the production parsers.
    // When a classic/legacy payload carries confirm:false the gate still
    // buffers; confirm:true emits immediately.

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

    if (open_sv.empty() || high_sv.empty() || low_sv.empty() || close_sv.empty())
        return std::nullopt;

    provider::bar b;
    b.symbol.assign(symbol.data(), symbol.size());

    double v = 0.0;
    if (!detail::parse_double_sv(open_sv, v))  return std::nullopt;
    b.open = v;
    if (!detail::parse_double_sv(high_sv, v))  return std::nullopt;
    b.high = v;
    if (!detail::parse_double_sv(low_sv, v))   return std::nullopt;
    b.low = v;
    if (!detail::parse_double_sv(close_sv, v)) return std::nullopt;
    b.close = v;

    if (!vol_sv.empty() && detail::parse_double_sv(vol_sv, v))
        b.volume = static_cast<int64_t>(v * 1e8);
    else
        b.volume = 0;

    if (!start_sv.empty())
        b.date.assign(start_sv.data(), start_sv.size());
    else if (!interval.empty())
        b.date.assign(interval.data(), interval.size());

    return b;
}

// Optional confirm flag on kline body (classic/legacy). UTA has no confirm.
inline std::optional<bool> extract_kline_confirm(std::string_view json)
{
    auto obj = detail::first_data_object(json);
    std::string_view body = obj.empty() ? json : obj;
    if (auto conf = extract_sv_optional_bool(body, "confirm"))
        return conf;
    return extract_sv_optional_bool(json, "confirm");
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
                                        std::optional<bool> confirm = std::nullopt)
    {
        if (confirm.has_value())
        {
            if (!*confirm)
            {
                pending_ = std::move(b);
                return std::nullopt;
            }
            // Explicit closed candle — emit now; clear pending for that start.
            pending_.reset();
            return b;
        }

        if (!pending_)
        {
            pending_ = std::move(b);
            return std::nullopt;
        }
        if (b.date == pending_->date)
        {
            pending_ = std::move(b); // in-progress update
            return std::nullopt;
        }
        auto closed = std::move(*pending_);
        pending_ = std::move(b);
        return closed;
    }

    void reset() { pending_.reset(); }

    const std::optional<provider::bar>& pending() const { return pending_; }

private:
    std::optional<provider::bar> pending_;
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
    return rec;
}

inline std::optional<bar_record> parse_kline_record(std::string_view json)
{
    auto b = parse_kline(json);
    if (!b) return std::nullopt;
    return to_bar_record(*b);
}

// ---------------------------------------------------------------------------
// Combined dispatcher (arg.topic → event)
// ---------------------------------------------------------------------------
// Single-event surface for IDataParser<provider::event>. For publicTrade,
// parse_ws_message returns the first trade only; BitgetCombinedParser
// overrides parse_records to emit the full data[] batch.
//
// Kline: raw parse only (no closed-bar gate). Production parsers apply
// kline_closed_gate so open-candle updates are not treated as completed bars.

inline std::optional<provider::event> parse_ws_message(std::string_view json)
{
    auto arg = detail::extract_object(json, "arg");
    std::string_view topic;
    if (!arg.empty())
        topic = extract_sv_string(arg, "topic");

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
    return gate.on_bar(std::move(*b), extract_kline_confirm(json));
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
        return bitget::parse_trade_record(std::string_view{line});
    }

    std::optional<tick_record> parse_record(std::string_view line) override
    {
        return bitget::parse_trade_record(line);
    }

    std::vector<tick_record> parse_records(std::string_view line) override
    {
        return bitget::parse_all_trade_records(line);
    }
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
        auto closed = bitget::gated_kline_bar(gate_, line);
        if (!closed) return std::nullopt;
        return bitget::to_bar_record(*closed);
    }

private:
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
// kline path uses closed-bar gate (same as BitgetKlineParser).
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
        auto arg = bitget::detail::extract_object(line, "arg");
        std::string_view topic;
        if (!arg.empty())
            topic = bitget::extract_sv_string(arg, "topic");

        if (topic == "publicTrade" || topic.empty())
        {
            auto ticks = bitget::parse_all_trades(line);
            if (!ticks.empty())
            {
                out.reserve(ticks.size());
                for (auto& t : ticks)
                    out.emplace_back(std::move(t));
                return out;
            }
        }

        if (topic == "kline")
        {
            auto closed = bitget::gated_kline_bar(kline_gate_, line);
            if (closed)
                out.emplace_back(std::move(*closed));
            return out;
        }

        // Empty topic: if frame parses as kline, apply gate (do not fall
        // through to raw parse_ws_message which would emit open candles).
        if (topic.empty())
        {
            if (auto raw = bitget::parse_kline(line))
            {
                auto closed = kline_gate_.on_bar(
                    std::move(*raw), bitget::extract_kline_confirm(line));
                if (closed)
                    out.emplace_back(std::move(*closed));
                return out;
            }
        }

        if (auto ev = bitget::parse_ws_message(line))
            out.push_back(std::move(*ev));
        return out;
    }

private:
    bitget::kline_closed_gate kline_gate_;
};

#endif // HAS_BITGET
