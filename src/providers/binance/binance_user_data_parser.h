#pragma once

#include "../../execution/fill_parser.h"
#include "binance_parser.h"

#include <chrono>
#include <charconv>
#include <cmath>
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
        out.venue_execution_id = take_id(json, "t");

        auto side = binance::extract_string(json, "S");
        const bool side_valid = side == "BUY" || side == "SELL";
        out.side = (side == "SELL") ? order_side::sell : order_side::buy;

        const auto last_qty = binance::extract_string(json, "l");
        const auto last_price = binance::extract_string(json, "L");
        const auto cumulative = binance::extract_string(json, "z");
        const auto commission = binance::extract_string(json, "n");
        const bool numeric_valid =
            read_nonnegative(last_qty, out.last_fill_qty)
            && read_nonnegative(last_price, out.last_fill_price)
            && read_nonnegative(cumulative, out.cumulative_qty)
            && read_finite_optional(commission, out.commission);
        out.has_cumulative_qty = !cumulative.empty();
        out.commission_asset = binance::extract_string(json, "N");

        auto ts_ms = binance::extract_number(json, "E");
        std::int64_t event_time_ms = 0;
        const bool timestamp_valid = !ts_ms.empty()
            && parse_int64(ts_ms, event_time_ms) && event_time_ms > 0;
        if (timestamp_valid)
        {
            out.ts = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(event_time_ms));
        }

        auto x_str = binance::extract_string(json, "x");
        auto X_str = binance::extract_string(json, "X");

        out.k = classify(x_str, X_str);

        const bool is_fill = out.k == parsed_exec::kind::partial_fill
            || out.k == parsed_exec::kind::full_fill;
        if (!numeric_valid || !side_valid || !timestamp_valid
            || out.symbol.empty() || out.client_order_id.empty()
            || out.exchange_order_id.empty()
            || (is_fill && (out.venue_execution_id.empty()
                            || !out.has_cumulative_qty
                            || !(out.last_fill_qty > 0.0)
                            || !(out.last_fill_price > 0.0)
                            || out.cumulative_qty < out.last_fill_qty)))
        {
            out.k = parsed_exec::kind::invalid;
            out.error = "malformed executionReport";
            return true;
        }

        if (out.k == parsed_exec::kind::rejected)
            out.error = binance::extract_string(json, "r");

        return true;
    }

private:
    static bool parse_int64(std::string_view text, std::int64_t& out) noexcept
    {
        const auto [end, ec] = std::from_chars(
            text.data(), text.data() + text.size(), out);
        return ec == std::errc{} && end == text.data() + text.size();
    }

    static bool parse_double(std::string_view text, double& out) noexcept
    {
        const auto [end, ec] = std::from_chars(
            text.data(), text.data() + text.size(), out,
            std::chars_format::general);
        return ec == std::errc{} && end == text.data() + text.size()
            && std::isfinite(out);
    }

    static bool read_nonnegative(std::string_view text, double& out) noexcept
    {
        if (text.empty())
        {
            out = 0.0;
            return true;
        }
        return parse_double(text, out) && out >= 0.0;
    }

    static bool read_finite_optional(std::string_view text, double& out) noexcept
    {
        if (text.empty())
        {
            out = 0.0;
            return true;
        }
        return parse_double(text, out);
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
            return parsed_exec::kind::invalid;
        }
        if (x == "NEW" && X == "NEW")
            return parsed_exec::kind::ack;
        if (x == "CANCELED" && X == "CANCELED")
            return parsed_exec::kind::canceled;
        if (x == "REJECTED" && X == "REJECTED")
            return parsed_exec::kind::rejected;
        if (x == "EXPIRED"
            && (X == "EXPIRED" || X == "EXPIRED_IN_MATCH"))
            return parsed_exec::kind::expired;
        if (x == "TRADE_PREVENTION"
            && (X == "EXPIRED" || X == "EXPIRED_IN_MATCH"))
            return parsed_exec::kind::expired;
        return parsed_exec::kind::invalid;
    }
};
