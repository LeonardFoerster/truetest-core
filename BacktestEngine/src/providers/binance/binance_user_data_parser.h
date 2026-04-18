#pragma once

#include "../../execution/fill_parser.h"
#include "binance_parser.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>

class BinanceUserDataParser : public IFillParser
{
public:
    bool parse(std::string_view raw, parsed_exec& out) override
    {
        std::string json(raw);

        auto event_type = binance::extract_string(json, "e");
        if (event_type != "executionReport")
            return false;

        out = parsed_exec{};

        out.symbol            = binance::extract_string(json, "s");
        out.client_order_id   = binance::extract_string(json, "c");
        out.exchange_order_id = take_id(json, "i");

        auto side = binance::extract_string(json, "S");
        out.side = (side == "SELL") ? order_side::sell : order_side::buy;

        out.last_fill_qty    = to_double(binance::extract_string(json, "l"));
        out.last_fill_price  = to_double(binance::extract_string(json, "L"));
        out.cumulative_qty   = to_double(binance::extract_string(json, "z"));
        out.commission       = to_double(binance::extract_string(json, "n"));
        out.commission_asset = binance::extract_string(json, "N");

        auto ts_ms = binance::extract_number(json, "E");
        if (!ts_ms.empty())
        {
            auto ms = std::strtoll(ts_ms.c_str(), nullptr, 10);
            out.ts = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(ms));
        }

        auto x_str = binance::extract_string(json, "x");
        auto X_str = binance::extract_string(json, "X");

        out.k = classify(x_str, X_str);

        if (out.k == parsed_exec::kind::rejected)
            out.error = binance::extract_string(json, "r");

        return true;
    }

private:
    static double to_double(const std::string& s)
    {
        if (s.empty()) return 0.0;
        return std::strtod(s.c_str(), nullptr);
    }

    static std::string take_id(const std::string& json, const std::string& key)
    {
        auto n = binance::extract_number(json, key);
        if (!n.empty()) return n;
        return binance::extract_string(json, key);
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
