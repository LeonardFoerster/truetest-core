#pragma once
#ifdef HAS_GATE

// Low-level Gate field extractors + trade/book primitives.
// Full channel dispatch lives in gate_combined_parser.h. Hand-rolled only.

#include "providers/gate/gate_json_util.h"
#include "providers/provider_event.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gate {

// ── Thin wrappers over json_util ──────────────────────────────────────────

inline std::string_view extract_sv_string(std::string_view json,
                                          std::string_view key)
{
    return json_util::extract(json, key);
}

inline std::string_view extract_sv_number(std::string_view json,
                                          std::string_view key)
{
    return json_util::extract(json, key);
}

inline bool extract_sv_bool(std::string_view json, std::string_view key)
{
    auto v = json_util::extract(json, key);
    return v == "true" || v == "1";
}

inline std::optional<bool> extract_sv_optional_bool(std::string_view json,
                                                    std::string_view key)
{
    auto colon = json_util::find_key(json, key);
    if (colon == std::string_view::npos) return std::nullopt;
    auto v = json_util::value_at_colon(json, colon);
    if (v == "true" || v == "1") return true;
    if (v == "false" || v == "0") return false;
    return std::nullopt;
}

inline bool parse_double_sv(std::string_view sv, double& out)
{
    return json_util::parse_double_sv(sv, out);
}

inline bool parse_int64_sv(std::string_view sv, int64_t& out)
{
    return json_util::parse_int64_sv(sv, out);
}

// Gate contract symbols are underscore form: BTC_USDT (G2).
// Accept already-canonical or bare BTCUSDT → BTC_USDT (best-effort).
inline std::string normalize_contract_symbol(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size() + 1);
    for (unsigned char c : raw)
        out.push_back(static_cast<char>(
            (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c));

    if (out.find('_') != std::string::npos)
        return out;

    // Heuristic: insert '_' before common quote currencies.
    static constexpr const char* quotes[] = {
        "USDT", "USDC", "USD", "BTC", "ETH"
    };
    for (const char* q : quotes)
    {
        const std::size_t qlen = std::char_traits<char>::length(q);
        if (out.size() > qlen
            && out.compare(out.size() - qlen, qlen, q) == 0)
        {
            out.insert(out.size() - qlen, 1, '_');
            return out;
        }
    }
    return out;
}

// ── Time helpers ──────────────────────────────────────────────────────────

inline std::chrono::system_clock::time_point tp_from_ms(int64_t ts_ms)
{
    return std::chrono::system_clock::time_point(
        std::chrono::milliseconds(ts_ms));
}

inline std::chrono::system_clock::time_point tp_from_s(int64_t ts_s)
{
    return std::chrono::system_clock::time_point(
        std::chrono::seconds(ts_s));
}

// Prefer create_time_ms / t (ms). Fall back to create_time / time (s).
inline std::chrono::system_clock::time_point
parse_trade_timestamp(std::string_view obj)
{
    auto ms_sv = extract_sv_number(obj, "create_time_ms");
    if (ms_sv.empty())
        ms_sv = extract_sv_number(obj, "t");
    int64_t ms = 0;
    if (!ms_sv.empty() && parse_int64_sv(ms_sv, ms) && ms > 1'000'000'000'000LL)
        return tp_from_ms(ms);

    // create_time is seconds; t may also be seconds if small.
    auto s_sv = extract_sv_number(obj, "create_time");
    if (s_sv.empty() && !ms_sv.empty())
        s_sv = ms_sv;
    int64_t s = 0;
    if (!s_sv.empty() && parse_int64_sv(s_sv, s))
    {
        if (s > 1'000'000'000'000LL)
            return tp_from_ms(s);
        return tp_from_s(s);
    }
    return std::chrono::system_clock::now();
}

// Engine qty is int64 with 1e8 scale (matches qty_scale default).
inline int64_t scale_qty(double abs_size)
{
    return static_cast<int64_t>(std::llround(std::abs(abs_size) * 1e8));
}

// Signed size → aggressor side (Binance/Bitget semantics).
// size > 0 = buy / bid (0); size < 0 = sell / ask (1).
inline uint8_t side_from_signed_size(double size)
{
    if (size > 0.0) return 0;
    if (size < 0.0) return 1;
    return 2;
}

// ── Trade object ──────────────────────────────────────────────────────────

// Parse one futures.trades result element. Drops is_internal==true.
inline std::optional<provider::tick>
parse_trade_object(std::string_view obj)
{
    // is_internal filter (G — no normal match).
    if (auto internal = extract_sv_optional_bool(obj, "is_internal"))
    {
        if (*internal)
            return std::nullopt;
    }

    auto price_sv = extract_sv_string(obj, "price");
    if (price_sv.empty())
        price_sv = extract_sv_number(obj, "price");
    auto size_sv = extract_sv_number(obj, "size");
    if (size_sv.empty())
        size_sv = extract_sv_string(obj, "size");
    auto contract = extract_sv_string(obj, "contract");
    if (contract.empty())
        contract = extract_sv_string(obj, "s");

    if (price_sv.empty() || size_sv.empty() || contract.empty())
        return std::nullopt;

    double price = 0.0;
    double size  = 0.0;
    if (!json_util::parse_numberish(price_sv, price)) return std::nullopt;
    if (!json_util::parse_numberish(size_sv, size)) return std::nullopt;
    if (!(price > 0.0) || size == 0.0) return std::nullopt;

    provider::tick t;
    t.symbol.assign(contract.data(), contract.size());
    t.price = price;
    t.quantity = scale_qty(size);
    t.side = side_from_signed_size(size);
    t.timestamp = parse_trade_timestamp(obj);
    return t;
}

// Parse futures.trades WS frame → all ticks in result[].
inline std::vector<provider::tick> parse_all_trades(std::string_view json)
{
    std::vector<provider::tick> out;
    auto channel = extract_sv_string(json, "channel");
    if (!channel.empty()
        && channel != "futures.trades"
        && channel != "futures.trade")
        return out;

    // Subscribe / error frames have no trade result objects.
    auto event = extract_sv_string(json, "event");
    if (event == "subscribe" || event == "unsubscribe")
        return out;

    auto arr = json_util::extract_array(json, "result");
    if (!arr.empty())
    {
        json_util::for_each_array_object(arr, [&](std::string_view obj) {
            if (auto t = parse_trade_object(obj))
                out.push_back(std::move(*t));
        });
        return out;
    }

    // Bare trade object (fixtures / REST).
    if (auto t = parse_trade_object(json))
        out.push_back(std::move(*t));
    return out;
}

// ── Order book levels (WS: [{p,s}], REST: [["px","sz"]]) ──────────────────

inline void append_ws_levels(
    std::string_view body,
    std::string_view key,
    std::vector<provider::l2_snapshot::level>& out)
{
    auto arr = json_util::extract_array(body, key);
    if (arr.size() < 2) return;

    if (out.capacity() < out.size() + 8)
        out.reserve(out.size() + 8);

    json_util::for_each_array_object(arr, [&](std::string_view obj) {
        auto p_sv = extract_sv_string(obj, "p");
        if (p_sv.empty())
            p_sv = extract_sv_number(obj, "p");
        auto s_sv = extract_sv_string(obj, "s");
        if (s_sv.empty())
            s_sv = extract_sv_number(obj, "s");
        if (p_sv.empty() || s_sv.empty()) return;

        double price = 0.0, size = 0.0;
        if (!json_util::parse_numberish(p_sv, price)) return;
        if (!json_util::parse_numberish(s_sv, size)) return;
        // Absolute size; s==0 → delete (quantity 0).
        out.push_back({price, scale_qty(size)});
    });
}

// REST order_book levels: [["price","size"], ...]
inline void append_rest_levels(
    std::string_view body,
    std::string_view key,
    std::vector<provider::l2_snapshot::level>& out)
{
    auto arr = json_util::extract_array(body, key);
    if (arr.size() < 2) return;

    std::size_t pos = 1;
    const std::size_t n = arr.size();
    while (pos < n)
    {
        json_util::skip_ws(arr, pos);
        if (pos >= n || arr[pos] == ']') break;
        if (arr[pos] == ',') { ++pos; continue; }
        if (arr[pos] != '[') break;
        auto close = json_util::matching_close(arr, pos);
        if (close == std::string_view::npos) break;
        std::string_view row = arr.substr(pos + 1, close - pos - 1);
        pos = close + 1;

        // Split two fields (quoted or bare).
        std::string_view fields[2];
        int fi = 0;
        std::size_t s = 0;
        bool in_q = false;
        for (std::size_t i = 0; i <= row.size() && fi < 2; ++i)
        {
            if (i < row.size() && row[i] == '"')
            {
                in_q = !in_q;
                continue;
            }
            if (i == row.size() || (!in_q && row[i] == ','))
            {
                auto f = row.substr(s, i - s);
                while (!f.empty()
                       && (f.front() == ' ' || f.front() == '\t'))
                    f.remove_prefix(1);
                while (!f.empty()
                       && (f.back() == ' ' || f.back() == '\t'))
                    f.remove_suffix(1);
                if (f.size() >= 2 && f.front() == '"' && f.back() == '"')
                    f = f.substr(1, f.size() - 2);
                fields[fi++] = f;
                s = i + 1;
            }
        }
        if (fi < 2) continue;
        double price = 0.0, size = 0.0;
        if (!json_util::parse_numberish(fields[0], price)) continue;
        if (!json_util::parse_numberish(fields[1], size)) continue;
        out.push_back({price, scale_qty(size)});
    }
}

// Parse futures.order_book_update result object → l2_snapshot.
// full:true or first snapshot-like frame → full book replace semantics
// (caller still applies U/u gap policy via depth_sync).
inline std::optional<provider::l2_snapshot>
parse_order_book_update_object(std::string_view obj)
{
    auto symbol = extract_sv_string(obj, "s");
    if (symbol.empty())
        symbol = extract_sv_string(obj, "contract");
    if (symbol.empty())
        return std::nullopt;

    provider::l2_snapshot snap;
    snap.symbol.assign(symbol.data(), symbol.size());

    auto t_sv = extract_sv_number(obj, "t");
    int64_t t_ms = 0;
    if (!t_sv.empty() && parse_int64_sv(t_sv, t_ms))
        snap.timestamp = tp_from_ms(t_ms);
    else
        snap.timestamp = std::chrono::system_clock::now();

    append_ws_levels(obj, "b", snap.bids);
    if (snap.bids.empty())
        append_ws_levels(obj, "bids", snap.bids);
    append_ws_levels(obj, "a", snap.asks);
    if (snap.asks.empty())
        append_ws_levels(obj, "asks", snap.asks);

    if (snap.bids.empty() && snap.asks.empty())
        return std::nullopt;
    return snap;
}

// Extract U/u/full from order_book_update result (for depth_sync).
struct book_update_meta
{
    int64_t U = 0;
    int64_t u = 0;
    bool full = false;
    bool ok = false;
};

inline book_update_meta extract_book_update_meta(std::string_view obj)
{
    book_update_meta m;
    auto U_sv = extract_sv_number(obj, "U");
    auto u_sv = extract_sv_number(obj, "u");
    if (U_sv.empty() || u_sv.empty()) return m;
    if (!parse_int64_sv(U_sv, m.U) || !parse_int64_sv(u_sv, m.u))
        return m;
    if (auto f = extract_sv_optional_bool(obj, "full"))
        m.full = *f;
    m.ok = true;
    return m;
}

// Parse futures.order_book_update WS frame.
inline std::optional<provider::l2_snapshot>
parse_order_book_update(std::string_view json)
{
    auto channel = extract_sv_string(json, "channel");
    if (!channel.empty() && channel != "futures.order_book_update"
        && channel != "futures.order_book"
        && channel != "futures.book_ticker")
    {
        // Allow bare result objects without channel.
        if (json.find("\"U\"") == std::string_view::npos
            && json.find("\"b\"") == std::string_view::npos)
            return std::nullopt;
    }

    auto event = extract_sv_string(json, "event");
    if (event == "subscribe" || event == "unsubscribe")
        return std::nullopt;

    auto result_obj = json_util::extract_object(json, "result");
    if (!result_obj.empty())
        return parse_order_book_update_object(result_obj);

    // result may be a single object without nested extract if top-level.
    return parse_order_book_update_object(json);
}

// Emit per-level l2_update events from a book update object (incremental).
inline std::vector<provider::l2_update>
parse_order_book_updates_as_deltas(std::string_view obj)
{
    std::vector<provider::l2_update> out;
    auto symbol = extract_sv_string(obj, "s");
    if (symbol.empty())
        symbol = extract_sv_string(obj, "contract");
    if (symbol.empty()) return out;

    auto t_sv = extract_sv_number(obj, "t");
    int64_t t_ms = 0;
    std::chrono::system_clock::time_point ts =
        std::chrono::system_clock::now();
    if (!t_sv.empty() && parse_int64_sv(t_sv, t_ms))
        ts = tp_from_ms(t_ms);

    auto emit_side = [&](std::string_view key, uint8_t side) {
        auto arr = json_util::extract_array(obj, key);
        json_util::for_each_array_object(arr, [&](std::string_view level) {
            auto p_sv = extract_sv_string(level, "p");
            if (p_sv.empty())
                p_sv = extract_sv_number(level, "p");
            auto s_sv = extract_sv_string(level, "s");
            if (s_sv.empty())
                s_sv = extract_sv_number(level, "s");
            double price = 0.0, size = 0.0;
            if (!json_util::parse_numberish(p_sv, price)) return;
            if (!json_util::parse_numberish(s_sv, size)) return;
            provider::l2_update u;
            u.timestamp = ts;
            u.symbol.assign(symbol.data(), symbol.size());
            u.side = side;
            u.price = price;
            u.new_quantity = scale_qty(size);
            out.push_back(std::move(u));
        });
    };
    emit_side("b", 0); // bid
    emit_side("a", 1); // ask
    return out;
}

// ── Candlesticks ──────────────────────────────────────────────────────────

inline std::optional<provider::bar>
parse_candlestick_object(std::string_view obj, std::string_view fallback_symbol)
{
    auto o_sv = extract_sv_string(obj, "o");
    if (o_sv.empty()) o_sv = extract_sv_number(obj, "o");
    auto h_sv = extract_sv_string(obj, "h");
    if (h_sv.empty()) h_sv = extract_sv_number(obj, "h");
    auto l_sv = extract_sv_string(obj, "l");
    if (l_sv.empty()) l_sv = extract_sv_number(obj, "l");
    auto c_sv = extract_sv_string(obj, "c");
    if (c_sv.empty()) c_sv = extract_sv_number(obj, "c");
    auto v_sv = extract_sv_string(obj, "v");
    if (v_sv.empty()) v_sv = extract_sv_number(obj, "v");
    auto t_sv = extract_sv_number(obj, "t");
    if (t_sv.empty()) t_sv = extract_sv_string(obj, "t");

    if (o_sv.empty() || h_sv.empty() || l_sv.empty() || c_sv.empty())
        return std::nullopt;

    provider::bar b;
    double v = 0.0;
    if (!json_util::parse_numberish(o_sv, v)) return std::nullopt;
    b.open = v;
    if (!json_util::parse_numberish(h_sv, v)) return std::nullopt;
    b.high = v;
    if (!json_util::parse_numberish(l_sv, v)) return std::nullopt;
    b.low = v;
    if (!json_util::parse_numberish(c_sv, v)) return std::nullopt;
    b.close = v;
    if (!v_sv.empty() && json_util::parse_numberish(v_sv, v))
        b.volume = scale_qty(v);
    else
        b.volume = 0;

    if (!t_sv.empty())
        b.date.assign(t_sv.data(), t_sv.size());

    // n field: "1m_BTC_USDT" → symbol BTC_USDT
    auto n_sv = extract_sv_string(obj, "n");
    if (!n_sv.empty())
    {
        auto us = n_sv.find('_');
        if (us != std::string_view::npos && us + 1 < n_sv.size())
        {
            // interval may contain digits; last two underscore parts = BASE_QUOTE
            auto last = n_sv.rfind('_');
            if (last != std::string_view::npos && last > 0)
            {
                auto prev = n_sv.rfind('_', last - 1);
                if (prev != std::string_view::npos)
                    b.symbol.assign(n_sv.substr(prev + 1).data(),
                                    n_sv.size() - prev - 1);
            }
        }
    }
    if (b.symbol.empty() && !fallback_symbol.empty())
        b.symbol.assign(fallback_symbol.data(), fallback_symbol.size());
    if (b.symbol.empty())
    {
        auto c = extract_sv_string(obj, "contract");
        if (!c.empty())
            b.symbol.assign(c.data(), c.size());
    }
    if (b.symbol.empty())
        return std::nullopt;
    return b;
}

inline std::optional<provider::bar>
parse_candlestick(std::string_view json)
{
    auto channel = extract_sv_string(json, "channel");
    if (!channel.empty()
        && channel != "futures.candlesticks"
        && channel != "futures.candle")
    {
        // bare object ok
    }

    auto event = extract_sv_string(json, "event");
    if (event == "subscribe" || event == "unsubscribe")
        return std::nullopt;

    auto result_obj = json_util::extract_object(json, "result");
    if (!result_obj.empty())
        return parse_candlestick_object(result_obj, {});

    auto result_arr = json_util::extract_array(json, "result");
    if (!result_arr.empty())
    {
        std::optional<provider::bar> first;
        json_util::for_each_array_object(result_arr, [&](std::string_view obj) {
            if (first) return;
            first = parse_candlestick_object(obj, {});
        });
        return first;
    }
    return parse_candlestick_object(json, {});
}

} // namespace gate

#endif // HAS_GATE
