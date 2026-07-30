#pragma once
#ifdef HAS_BYBIT

// Bybit V5 orderbook snapshot/delta → provider::l2_snapshot.
// Topic: orderbook.{depth}.{symbol}  (depth: 1, 50, 200, 1000)
// type=="snapshot" | type=="delta"; size "0" = level delete.

#include "providers/bybit/bybit_parser.h"
#include "providers/parser.h"
#include "providers/provider_event.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bybit {

// Parse orderbook envelope into l2_snapshot (partial levels for delta OK).
// Prefer cts (matching trades) over ts when present.
inline std::optional<provider::l2_snapshot> parse_orderbook(std::string_view json)
{
    auto topic = extract_sv_string(json, "topic");
    if (!topic.empty() && !detail::topic_is_orderbook(topic))
        return std::nullopt;

    auto type = extract_sv_string(json, "type");
    // Accept snapshot + delta (both as l2_snapshot-shaped partial books).
    if (!type.empty() && type != "snapshot" && type != "delta")
        return std::nullopt;

    // data is a single object for orderbook (not always array).
    auto data_obj = detail::extract_object(json, "data");
    std::string_view body = data_obj;
    if (body.empty())
    {
        // Some fixtures may wrap data as [{...}].
        auto first = detail::first_data_object(json);
        body = first.empty() ? json : first;
    }

    std::string_view symbol = extract_sv_string(body, "s");
    if (symbol.empty())
        symbol = extract_sv_string(body, "symbol");
    if (symbol.empty())
        symbol = detail::symbol_from_topic(topic);
    if (symbol.empty())
        return std::nullopt;

    provider::l2_snapshot snap;
    snap.symbol.assign(symbol.data(), symbol.size());

    // Prefer envelope cts (match trades), then data cts, then ts.
    auto ts_sv = extract_sv_number(json, "cts");
    if (ts_sv.empty())
        ts_sv = extract_sv_number(body, "cts");
    if (ts_sv.empty())
        ts_sv = extract_sv_number(body, "ts");
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

    // Delta may only touch one side; empty both is malformed.
    if (snap.bids.empty() && snap.asks.empty())
        return std::nullopt;

    return snap;
}

// Expand orderbook levels into per-level l2_update events.
inline std::vector<provider::l2_update> parse_orderbook_updates(std::string_view json)
{
    std::vector<provider::l2_update> updates;
    auto snap = parse_orderbook(json);
    if (!snap) return updates;

    updates.reserve(snap->bids.size() + snap->asks.size());
    auto push_side = [&](const std::vector<provider::l2_snapshot::level>& levels,
                         uint8_t side) {
        for (const auto& lvl : levels)
        {
            provider::l2_update upd;
            upd.timestamp = snap->timestamp;
            upd.symbol = snap->symbol;
            upd.side = side;
            upd.price = lvl.price;
            upd.new_quantity = lvl.quantity;
            updates.push_back(std::move(upd));
        }
    };
    push_side(snap->bids, /*bid=*/0);
    push_side(snap->asks, /*ask=*/1);
    return updates;
}

} // namespace bybit

class BybitDepthParser : public IDataParser<provider::l2_snapshot>
{
public:
    bool parse_header(const std::string&) override { return true; }

    std::optional<provider::l2_snapshot> parse_record(const std::string& line) override
    {
        return bybit::parse_orderbook(std::string_view{line});
    }

    std::optional<provider::l2_snapshot> parse_record(std::string_view line) override
    {
        return bybit::parse_orderbook(line);
    }
};

#endif // HAS_BYBIT
