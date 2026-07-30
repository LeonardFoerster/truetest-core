#pragma once
#ifdef HAS_BYBIT

// Multi-topic Bybit public WS → provider::event.
// publicTrade: full data[] via parse_records; kline: closed-bar gate.

#include "providers/bybit/bybit_depth_parser.h"
#include "providers/bybit/bybit_parser.h"
#include "providers/parser.h"
#include "providers/provider_event.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bybit {

// Single-event surface: first trade only for publicTrade.
inline std::optional<provider::event> parse_ws_message(std::string_view json)
{
    auto topic = extract_sv_string(json, "topic");

    if (detail::topic_is_public_trade(topic) || topic.empty())
    {
        if (auto t = parse_trade(json))
            return provider::event{std::move(*t)};
        if (!topic.empty())
            return std::nullopt;
    }
    if (detail::topic_is_orderbook(topic))
    {
        auto snap = parse_orderbook(json);
        if (!snap) return std::nullopt;
        return provider::event{std::move(*snap)};
    }
    if (detail::topic_is_kline(topic))
    {
        auto b = parse_kline(json);
        if (!b) return std::nullopt;
        return provider::event{std::move(*b)};
    }

    if (topic.empty())
    {
        if (auto snap = parse_orderbook(json))
            return provider::event{std::move(*snap)};
        if (auto b = parse_kline(json))
            return provider::event{std::move(*b)};
    }

    return std::nullopt;
}

} // namespace bybit

// Combined event adapter. publicTrade → full data[] via parse_records;
// kline path uses closed-bar gate (confirm:true or start rollover).
class BybitCombinedParser : public IDataParser<provider::event>
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
        auto topic = bybit::extract_sv_string(line, "topic");

        if (bybit::detail::topic_is_public_trade(topic) || topic.empty())
        {
            auto ticks = bybit::parse_all_trades(line);
            if (!ticks.empty())
            {
                out.reserve(ticks.size());
                for (auto& t : ticks)
                    out.emplace_back(std::move(t));
                return out;
            }
        }

        if (bybit::detail::topic_is_kline(topic))
        {
            auto closed = bybit::gated_kline_bar(kline_gate_, line);
            if (closed)
                out.emplace_back(std::move(*closed));
            return out;
        }

        if (topic.empty())
        {
            if (auto raw = bybit::parse_kline(line))
            {
                auto closed = kline_gate_.on_bar(
                    std::move(*raw), bybit::extract_kline_confirm(line));
                if (closed)
                    out.emplace_back(std::move(*closed));
                return out;
            }
        }

        if (auto ev = bybit::parse_ws_message(line))
            out.push_back(std::move(*ev));
        return out;
    }

private:
    bybit::kline_closed_gate kline_gate_;
};

#endif // HAS_BYBIT
