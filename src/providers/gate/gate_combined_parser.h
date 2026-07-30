#pragma once
#ifdef HAS_GATE

// Channel-dispatch parser: futures.trades / order_book_update / candlesticks
// → provider::event. Multi-trade frames emit via parse_records.
// Hand-rolled only — no nlohmann.

#include "providers/gate/gate_parser.h"
#include "providers/parser.h"
#include "providers/provider_event.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gate {

// Dispatch a single WS frame. Trades: first tick only (use parse_all_trades
// / GateCombinedParser::parse_records for full result[]).
inline std::optional<provider::event> parse_ws_message(std::string_view json)
{
    auto channel = extract_sv_string(json, "channel");
    auto event = extract_sv_string(json, "event");

    // Control / subscribe acks — no market event.
    if (event == "subscribe" || event == "unsubscribe")
        return std::nullopt;
    if (channel == "futures.pong" || channel == "futures.ping")
        return std::nullopt;

    if (channel == "futures.trades" || channel == "futures.trade"
        || channel.empty())
    {
        auto ticks = parse_all_trades(json);
        if (!ticks.empty())
            return provider::event{std::move(ticks.front())};
        if (!channel.empty() && channel != "futures.trades"
            && channel != "futures.trade")
        {
            // fall through to other channels when channel empty tried trade
        }
        else if (!channel.empty())
        {
            return std::nullopt;
        }
    }

    if (channel == "futures.order_book_update"
        || channel == "futures.order_book"
        || channel.empty())
    {
        // Prefer snapshot surface for full:true; deltas as l2_update batch
        // is handled in GateCombinedParser::parse_records.
        auto result_obj = json_util::extract_object(json, "result");
        std::string_view body =
            result_obj.empty() ? json : result_obj;
        auto meta = extract_book_update_meta(body);
        if (meta.ok || channel == "futures.order_book_update"
            || channel == "futures.order_book")
        {
            if (meta.full || !meta.ok)
            {
                if (auto snap = parse_order_book_update(json))
                    return provider::event{std::move(*snap)};
            }
            else
            {
                // Incremental: return first delta; parse_records emits all.
                auto deltas = parse_order_book_updates_as_deltas(body);
                if (!deltas.empty())
                    return provider::event{std::move(deltas.front())};
                // Fallback snapshot of changed levels.
                if (auto snap = parse_order_book_update_object(body))
                    return provider::event{std::move(*snap)};
            }
        }
    }

    if (channel == "futures.candlesticks" || channel == "futures.candle"
        || channel.empty())
    {
        if (auto b = parse_candlestick(json))
            return provider::event{std::move(*b)};
    }

    return std::nullopt;
}

} // namespace gate

// Production combined parser (IDataParser<provider::event>).
class GateCombinedParser : public IDataParser<provider::event>
{
public:
    bool parse_header(const std::string&) override { return true; }

    std::optional<provider::event>
    parse_record(const std::string& line) override
    {
        return parse_record(std::string_view{line});
    }

    std::optional<provider::event>
    parse_record(std::string_view line) override
    {
        auto batch = parse_records(line);
        if (batch.empty()) return std::nullopt;
        return std::move(batch.front());
    }

    std::vector<provider::event>
    parse_records(std::string_view line) override
    {
        std::vector<provider::event> out;

        auto channel = gate::extract_sv_string(line, "channel");
        auto event = gate::extract_sv_string(line, "event");
        if (event == "subscribe" || event == "unsubscribe")
            return out;
        if (channel == "futures.pong" || channel == "futures.ping")
            return out;

        // Trades: emit every result[] element (is_internal already filtered).
        if (channel.empty() || channel == "futures.trades"
            || channel == "futures.trade")
        {
            auto ticks = gate::parse_all_trades(line);
            if (!ticks.empty())
            {
                out.reserve(ticks.size());
                for (auto& t : ticks)
                    out.emplace_back(std::move(t));
                return out;
            }
            if (!channel.empty())
                return out;
        }

        // Order book update.
        if (channel.empty() || channel == "futures.order_book_update"
            || channel == "futures.order_book")
        {
            auto result_obj = gate::json_util::extract_object(line, "result");
            std::string_view body =
                result_obj.empty() ? line : result_obj;
            auto meta = gate::extract_book_update_meta(body);

            if (meta.ok || channel == "futures.order_book_update"
                || channel == "futures.order_book")
            {
                if (meta.full)
                {
                    if (auto snap = gate::parse_order_book_update_object(body))
                        out.emplace_back(std::move(*snap));
                    return out;
                }

                // Incremental: one l2_update per level change.
                auto deltas = gate::parse_order_book_updates_as_deltas(body);
                if (!deltas.empty())
                {
                    out.reserve(deltas.size());
                    for (auto& d : deltas)
                        out.emplace_back(std::move(d));
                    return out;
                }

                // No deltas but has levels → snapshot of changed book side.
                if (auto snap = gate::parse_order_book_update_object(body))
                    out.emplace_back(std::move(*snap));
                if (!channel.empty())
                    return out;
            }
        }

        // Candlesticks.
        if (channel.empty() || channel == "futures.candlesticks"
            || channel == "futures.candle")
        {
            if (auto b = gate::parse_candlestick(line))
                out.emplace_back(std::move(*b));
            return out;
        }

        if (auto ev = gate::parse_ws_message(line))
            out.push_back(std::move(*ev));
        return out;
    }
};

#endif // HAS_GATE
