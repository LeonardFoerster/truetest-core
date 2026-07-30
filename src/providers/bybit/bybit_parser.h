#pragma once
#ifdef HAS_BYBIT

// Bybit V5 public market-data hand parsers (zero nlohmann on hot path).
// Wire envelope: {"topic":"publicTrade.BTCUSDT","type":"…","ts":…,"data":[…]}

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

namespace bybit {

namespace detail {

inline void skip_ws(std::string_view json, std::size_t& pos)
{
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        ++pos;
}

// Locate `"key":` (no space between quote and colon — Bybit wire).
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

// Locate `"key":[` and append [price, size] string levels (qty * 1e8).
// size "0" → qty 0 (level delete).
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

// Bybit taker side → aggressor (Binance/Bitget convention).
// Buy-taker = buyer aggressor → bid (0); Sell-taker → ask (1).
inline std::optional<data_tick_side> map_side(std::string_view side)
{
    if (side == "Buy" || side == "buy" || side == "BUY")
        return data_tick_side::bid;
    if (side == "Sell" || side == "sell" || side == "SELL")
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

// topic "publicTrade.BTCUSDT" → "BTCUSDT"; "orderbook.50.BTCUSDT" → last segment.
inline std::string_view symbol_from_topic(std::string_view topic)
{
    auto dot = topic.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= topic.size())
        return {};
    return topic.substr(dot + 1);
}

// Topic family without symbol: "publicTrade", "orderbook.50", "kline.1".
inline std::string_view topic_family(std::string_view topic)
{
    // publicTrade.SYM → publicTrade
    if (topic.size() >= 12 && topic.substr(0, 11) == "publicTrade" &&
        (topic.size() == 11 || topic[11] == '.'))
        return topic.substr(0, 11);
    // orderbook.{depth}.SYM
    if (topic.size() >= 10 && topic.substr(0, 9) == "orderbook" &&
        (topic.size() == 9 || topic[9] == '.'))
    {
        // Keep orderbook.{depth} if two dots after orderbook.
        auto first = topic.find('.');
        if (first == std::string_view::npos) return topic.substr(0, 9);
        auto second = topic.find('.', first + 1);
        if (second == std::string_view::npos) return topic; // orderbook.50 only
        return topic.substr(0, second);
    }
    // kline.{interval}.SYM
    if (topic.size() >= 6 && topic.substr(0, 5) == "kline" &&
        (topic.size() == 5 || topic[5] == '.'))
    {
        auto first = topic.find('.');
        if (first == std::string_view::npos) return topic.substr(0, 5);
        auto second = topic.find('.', first + 1);
        if (second == std::string_view::npos) return topic;
        return topic.substr(0, second);
    }
    return topic;
}

inline bool topic_is_public_trade(std::string_view topic)
{
    return topic.size() >= 11 && topic.substr(0, 11) == "publicTrade";
}

inline bool topic_is_orderbook(std::string_view topic)
{
    return topic.size() >= 9 && topic.substr(0, 9) == "orderbook";
}

inline bool topic_is_kline(std::string_view topic)
{
    return topic.size() >= 5 && topic.substr(0, 5) == "kline";
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

// Parse one trade object (fields p/v/S/T/s). Symbol fallback from topic/envelope.
inline std::optional<provider::tick> parse_trade_object(std::string_view obj,
                                                        std::string_view symbol)
{
    std::string_view price_sv;
    std::string_view qty_sv;
    std::string_view side_sv;
    std::string_view time_sv;
    std::string_view sym_sv = symbol;

    detail::for_each_flat_field(obj, 0, obj.size(),
        [&](std::string_view key, std::string_view value) {
            if (key == "p") price_sv = value;
            else if (key == "v") qty_sv = value;
            else if (key == "S") side_sv = value;
            else if (key == "side" && side_sv.empty()) side_sv = value;
            else if (key == "T") time_sv = value;
            else if (key == "ts" && time_sv.empty()) time_sv = value;
            else if (key == "s" && sym_sv.empty()) sym_sv = value;
            else if (key == "symbol" && sym_sv.empty()) sym_sv = value;
        });

    if (sym_sv.empty() || price_sv.empty() || qty_sv.empty() || side_sv.empty())
        return std::nullopt;

    double price = 0.0, qty = 0.0;
    if (!detail::parse_double_sv(price_sv, price)) return std::nullopt;
    if (!detail::parse_double_sv(qty_sv, qty)) return std::nullopt;

    auto side = detail::map_side(side_sv);
    if (!side) return std::nullopt;

    provider::tick t;
    t.symbol.assign(sym_sv.data(), sym_sv.size());
    t.price = price;
    t.quantity = static_cast<int64_t>(qty * 1e8);
    t.side = detail::side_to_u8(*side);

    if (auto tp = detail::parse_ts_ms(time_sv))
        t.timestamp = *tp;
    else
        t.timestamp = std::chrono::system_clock::now();

    return t;
}

// Full publicTrade push → **first** trade in data[] only.
inline std::optional<provider::tick> parse_trade(std::string_view json)
{
    auto topic = extract_sv_string(json, "topic");
    if (!topic.empty() && !detail::topic_is_public_trade(topic))
        return std::nullopt;

    std::string_view symbol = detail::symbol_from_topic(topic);
    // Prefer data[0].s when present.

    auto obj = detail::first_data_object(json);
    if (obj.empty())
        return parse_trade_object(json, symbol);
    return parse_trade_object(obj, symbol);
}

// All trades in a publicTrade push (data[]).
inline std::vector<provider::tick> parse_all_trades(std::string_view json)
{
    std::vector<provider::tick> out;
    auto topic = extract_sv_string(json, "topic");
    if (!topic.empty() && !detail::topic_is_public_trade(topic))
        return out;

    std::string_view symbol = detail::symbol_from_topic(topic);

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

inline std::vector<tick_record> parse_all_trade_records(std::string_view json)
{
    std::vector<tick_record> out;
    auto ticks = parse_all_trades(json);
    out.reserve(ticks.size());
    for (const auto& t : ticks)
        out.push_back(tick_to_record(t));
    if (out.empty())
    {
        if (auto one = parse_trade_record(json))
            out.push_back(std::move(*one));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Kline → bar
// ---------------------------------------------------------------------------

// Bybit kline data[] object fields: start, open, high, low, close, volume,
// confirm (bool). Only emit closed candles when confirm==true in gated path.
inline std::optional<provider::bar> parse_kline(std::string_view json)
{
    auto topic = extract_sv_string(json, "topic");
    if (!topic.empty() && !detail::topic_is_kline(topic))
        return std::nullopt;

    std::string_view symbol = detail::symbol_from_topic(topic);

    auto obj = detail::first_data_object(json);
    std::string_view body = obj.empty() ? json : obj;

    if (symbol.empty())
        symbol = extract_sv_string(body, "symbol");
    if (symbol.empty())
        return std::nullopt;

    std::string_view open_sv, high_sv, low_sv, close_sv, vol_sv, start_sv;

    detail::for_each_flat_field(body, 0, body.size(),
        [&](std::string_view key, std::string_view value) {
            if (key == "open") open_sv = value;
            else if (key == "high") high_sv = value;
            else if (key == "low") low_sv = value;
            else if (key == "close") close_sv = value;
            else if (key == "volume") vol_sv = value;
            else if (key == "start") start_sv = value;
            else if (key == "o" && open_sv.empty()) open_sv = value;
            else if (key == "h" && high_sv.empty()) high_sv = value;
            else if (key == "l" && low_sv.empty()) low_sv = value;
            else if (key == "c" && close_sv.empty()) close_sv = value;
            else if (key == "v" && vol_sv.empty()) vol_sv = value;
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

    return b;
}

inline std::optional<bool> extract_kline_confirm(std::string_view json)
{
    auto obj = detail::first_data_object(json);
    std::string_view body = obj.empty() ? json : obj;
    if (auto conf = extract_sv_optional_bool(body, "confirm"))
        return conf;
    return extract_sv_optional_bool(json, "confirm");
}

// Closed-bar policy: confirm:true → emit; confirm:false → hold;
// confirm absent → start rollover gate (same as Bitget UTA path).
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
            pending_ = std::move(b);
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
    return bybit::to_bar_record(*b);
}

inline std::optional<provider::bar>
gated_kline_bar(kline_closed_gate& gate, std::string_view json)
{
    auto b = parse_kline(json);
    if (!b) return std::nullopt;
    return gate.on_bar(std::move(*b), extract_kline_confirm(json));
}

} // namespace bybit

// IDataParser adapters (outside namespace).

class BybitTradeParser : public IDataParser<tick_record>
{
public:
    bool parse_header(const std::string&) override { return true; }

    std::optional<tick_record> parse_record(const std::string& line) override
    {
        return bybit::parse_trade_record(std::string_view{line});
    }

    std::optional<tick_record> parse_record(std::string_view line) override
    {
        return bybit::parse_trade_record(line);
    }

    std::vector<tick_record> parse_records(std::string_view line) override
    {
        return bybit::parse_all_trade_records(line);
    }
};

class BybitKlineParser : public IDataParser<bar_record>
{
public:
    bool parse_header(const std::string&) override { return true; }

    std::optional<bar_record> parse_record(const std::string& line) override
    {
        return parse_record(std::string_view{line});
    }

    std::optional<bar_record> parse_record(std::string_view line) override
    {
        auto closed = bybit::gated_kline_bar(gate_, line);
        if (!closed) return std::nullopt;
        return bybit::to_bar_record(*closed);
    }

private:
    bybit::kline_closed_gate gate_;
};

#endif // HAS_BYBIT
