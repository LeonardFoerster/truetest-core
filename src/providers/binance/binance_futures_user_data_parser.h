#pragma once

#include "../../execution/fill_parser.h"
#include "binance_parser.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>

// USDT-M futures user-data stream wraps execution detail in
// `ORDER_TRADE_UPDATE` with a nested `o:{...}`. The inner field codes
// overlap with spot's flat `executionReport`, so we slice the inner
// object and reuse the same `binance::extract_*` helpers.
// Wrapper keys we read top-level: `e` (event guard), `E` (event time).
// Everything else lives one nesting level down. The wrapper key `o`
// itself collides with the inner type field of the same name, so we
// never `extract` `o` after slicing — only the bracketing find.
class BinanceFuturesUserDataParser : public IFillParser
{
public:
    bool parse_position_snapshot(std::string_view raw,
                                 parsed_position_snapshot& out) override
    {
        std::string json(raw);

        auto event_type = binance::extract_string(json, "e");
        if (event_type != "ACCOUNT_UPDATE")
            return false;

        auto a_pos = json.find("\"a\":{");
        if (a_pos == std::string::npos) return false;
        std::string_view inner(json.data() + a_pos, json.size() - a_pos);

        out = parsed_position_snapshot{};

        auto m_str = std::string(binance::extract_sv_string(inner, "m"));
        out.r = classify_reason(m_str);

        auto ts_ms = binance::extract_number(json, "E");
        if (!ts_ms.empty())
        {
            auto ms = std::strtoll(ts_ms.c_str(), nullptr, 10);
            out.ts = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(ms));
        }

        // Walk B[] (balances) — array starts at "B":[ inside the inner.
        for_each_object_in_array(inner, "B", [&](std::string_view obj) {
            parsed_position_snapshot::balance_row row;
            row.asset = std::string(binance::extract_sv_string(obj, "a"));
            row.wallet_balance =
                to_double(binance::extract_sv_string(obj, "wb"));
            row.balance_change =
                to_double(binance::extract_sv_string(obj, "bc"));
            if (!row.asset.empty())
                out.balances.push_back(std::move(row));
        });

        // Walk P[] (positions).
        for_each_object_in_array(inner, "P", [&](std::string_view obj) {
            parsed_position_snapshot::position_row row;
            row.symbol = std::string(binance::extract_sv_string(obj, "s"));
            row.qty    = to_double(binance::extract_sv_string(obj, "pa"));
            auto mt    = binance::extract_sv_string(obj, "mt");
            row.margin_type = canonical_margin_type(mt);
            row.position_side =
                std::string(binance::extract_sv_string(obj, "ps"));
            if (!row.symbol.empty())
                out.positions.push_back(std::move(row));
        });

        return true;
    }

    bool parse(std::string_view raw, parsed_exec& out) override
    {
        std::string json(raw);

        auto event_type = binance::extract_string(json, "e");
        if (event_type != "ORDER_TRADE_UPDATE")
            return false;

        auto o_pos = json.find("\"o\":{");
        if (o_pos == std::string::npos) return false;
        std::string_view inner(json.data() + o_pos, json.size() - o_pos);

        out = parsed_exec{};

        out.symbol            = std::string(binance::extract_sv_string(inner, "s"));
        out.client_order_id   = std::string(binance::extract_sv_string(inner, "c"));
        out.exchange_order_id = take_id(inner, "i");

        auto side = binance::extract_sv_string(inner, "S");
        out.side = (side == "SELL") ? order_side::sell : order_side::buy;

        out.last_fill_qty    = to_double(binance::extract_sv_string(inner, "l"));
        out.last_fill_price  = to_double(binance::extract_sv_string(inner, "L"));
        out.cumulative_qty   = to_double(binance::extract_sv_string(inner, "z"));
        out.commission       = to_double(binance::extract_sv_string(inner, "n"));
        out.commission_asset = std::string(binance::extract_sv_string(inner, "N"));

        // Wrapper-level event time (matches spot, which uses `E`).
        auto ts_ms = binance::extract_number(json, "E");
        if (!ts_ms.empty())
        {
            auto ms = std::strtoll(ts_ms.c_str(), nullptr, 10);
            out.ts = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(ms));
        }

        auto x_str = std::string(binance::extract_sv_string(inner, "x"));
        auto X_str = std::string(binance::extract_sv_string(inner, "X"));

        out.k = classify(x_str, X_str);

        if (out.k == parsed_exec::kind::rejected)
            out.error = std::string(binance::extract_sv_string(inner, "r"));

        return true;
    }

private:
    static double to_double(std::string_view sv)
    {
        if (sv.empty()) return 0.0;
        std::string s(sv);
        return std::strtod(s.c_str(), nullptr);
    }

    static std::string take_id(std::string_view inner, std::string_view key)
    {
        auto n = binance::extract_sv_number(inner, key);
        if (!n.empty()) return std::string(n);
        auto s = binance::extract_sv_string(inner, key);
        return std::string(s);
    }

    // Walk a JSON array nested under `array_key` inside `body`. Calls
    // `fn` once per top-level `{...}` object in the array. Brace-aware
    // so nested objects (rare in ACCOUNT_UPDATE entries but cheap to
    // handle) don't split prematurely.
    template <typename Fn>
    static void for_each_object_in_array(std::string_view body,
                                         std::string_view array_key,
                                         Fn fn)
    {
        std::string needle;
        needle.reserve(array_key.size() + 4);
        needle += '"';
        needle += array_key;
        needle += "\":[";
        auto start = body.find(needle);
        if (start == std::string_view::npos) return;
        std::size_t i = start + needle.size();

        while (i < body.size())
        {
            if (body[i] == ']') return;
            auto open = body.find('{', i);
            if (open == std::string_view::npos) return;
            int depth = 0;
            std::size_t j = open;
            for (; j < body.size(); ++j)
            {
                if (body[j] == '{') ++depth;
                else if (body[j] == '}')
                {
                    --depth;
                    if (depth == 0) { ++j; break; }
                }
            }
            fn(body.substr(open, j - open));
            i = j;
        }
    }

    // a.m → reason. Binance enumerates many; we collapse to a coarse
    // set the engine actually distinguishes. "ORDER" is folded into a
    // distinct value so a consumer can dedupe against ORDER_TRADE_UPDATE
    // (the same underlying fill drives both messages on a typical fill).
    static parsed_position_snapshot::reason
    classify_reason(const std::string& m)
    {
        if (m == "ORDER")              return parsed_position_snapshot::reason::order;
        if (m == "FUNDING_FEE")        return parsed_position_snapshot::reason::funding_fee;
        if (m == "ADJUSTMENT")         return parsed_position_snapshot::reason::adjustment;
        if (m == "DEPOSIT")            return parsed_position_snapshot::reason::deposit;
        if (m == "WITHDRAW")           return parsed_position_snapshot::reason::withdraw;
        if (m == "MARGIN_TRANSFER")    return parsed_position_snapshot::reason::margin_transfer;
        if (m == "MARGIN_TYPE_CHANGE") return parsed_position_snapshot::reason::margin_type_change;
        if (m == "INSURANCE_CLEAR")    return parsed_position_snapshot::reason::liquidation;
        if (m == "ADMIN_DEPOSIT" ||
            m == "ADMIN_WITHDRAW")     return parsed_position_snapshot::reason::admin;
        if (m.empty())                 return parsed_position_snapshot::reason::unknown;
        return parsed_position_snapshot::reason::other;
    }

    // Binance returns marginType as lowercase ("isolated" / "cross").
    // Canonicalize so downstream comparisons line up with the operator's
    // expected_margin_type (which compute_advisories also normalizes).
    static std::string canonical_margin_type(std::string_view sv)
    {
        if (sv.empty()) return {};
        char first = static_cast<char>(
            std::toupper(static_cast<unsigned char>(sv[0])));
        if (first == 'I') return "ISOLATED";
        if (first == 'C') return "CROSSED";
        return std::string(sv);
    }

    static parsed_exec::kind classify(const std::string& x,
                                      const std::string& X)
    {
        if (x == "TRADE")
        {
            if (X == "FILLED")           return parsed_exec::kind::full_fill;
            if (X == "PARTIALLY_FILLED") return parsed_exec::kind::partial_fill;
            return parsed_exec::kind::partial_fill;
        }
        if (x == "NEW")      return parsed_exec::kind::ack;
        if (x == "CANCELED") return parsed_exec::kind::canceled;
        if (x == "REJECTED") return parsed_exec::kind::rejected;
        if (x == "EXPIRED")  return parsed_exec::kind::expired;

        if (X == "CANCELED") return parsed_exec::kind::canceled;
        if (X == "REJECTED") return parsed_exec::kind::rejected;
        if (X == "EXPIRED")  return parsed_exec::kind::expired;

        return parsed_exec::kind::other;
    }
};
