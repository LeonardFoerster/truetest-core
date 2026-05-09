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
//
// Wrapper keys we read top-level: `e` (event guard), `E` (event time).
// Everything else lives one nesting level down. The wrapper key `o`
// itself collides with the inner type field of the same name, so we
// never `extract` `o` after slicing — only the bracketing find.
class BinanceFuturesUserDataParser : public IFillParser
{
public:
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
