#pragma once

#include "providers/parser.h"
#include "providers/provider_event.h"
#include "providers/binance/binance_parser.h"

#include <optional>
#include <string>
#include <vector>


namespace binance {

inline std::optional<provider::l2_snapshot> parse_depth_snapshot(const std::string& json)
{
    provider::l2_snapshot snap;

    snap.symbol = extract_string(json, "s");

    auto ts_str = extract_number(json, "E");
    if (!ts_str.empty())
    {
        int64_t ts_ms = std::stoll(ts_str);
        snap.timestamp = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(ts_ms));
    }
    else
    {
        snap.timestamp = std::chrono::system_clock::now();
    }

    auto parse_levels = [&](const std::string& key) -> std::vector<provider::l2_snapshot::level>
    {
        std::vector<provider::l2_snapshot::level> levels;

        std::string search = "\"" + key + "\":[";
        auto pos = json.find(search);
        if (pos == std::string::npos) return levels;
        pos += search.size();

        while (pos < json.size())
        {
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == ','))
                pos++;

            if (pos >= json.size() || json[pos] == ']')
                break;

            if (json[pos] != '[')
                break;
            pos++;

            if (pos >= json.size() || json[pos] != '"') break;
            pos++;
            auto price_end = json.find('"', pos);
            if (price_end == std::string::npos) break;
            std::string price_str = json.substr(pos, price_end - pos);
            pos = price_end + 1;

            while (pos < json.size() && (json[pos] == ',' || json[pos] == ' '))
                pos++;

            if (pos >= json.size() || json[pos] != '"') break;
            pos++;
            auto qty_end = json.find('"', pos);
            if (qty_end == std::string::npos) break;
            std::string qty_str = json.substr(pos, qty_end - pos);
            pos = qty_end + 1;

            while (pos < json.size() && json[pos] != ']') pos++;
            if (pos < json.size()) pos++;

            double price = std::stod(price_str);
            double qty = std::stod(qty_str);
            levels.push_back({price, static_cast<int64_t>(qty * 1e8)});

            while (pos < json.size() && (json[pos] == ',' || json[pos] == ' '))
                pos++;

            if (pos >= json.size() || json[pos] == ']')
                break;
        }

        return levels;
    };

    snap.bids = parse_levels("bids");
    if (snap.bids.empty())
        snap.bids = parse_levels("b");

    snap.asks = parse_levels("asks");
    if (snap.asks.empty())
        snap.asks = parse_levels("a");

    if (snap.bids.empty() && snap.asks.empty())
        return std::nullopt;

    return snap;
}

inline std::vector<provider::l2_update> parse_depth_updates(const std::string& json)
{
    std::vector<provider::l2_update> updates;

    auto snap_opt = parse_depth_snapshot(json);
    if (!snap_opt) return updates;
    auto& snap = *snap_opt;

    for (const auto& lvl : snap.bids)
    {
        provider::l2_update upd;
        upd.timestamp = snap.timestamp;
        upd.symbol = snap.symbol;
        upd.side = 0;
        upd.price = lvl.price;
        upd.new_quantity = lvl.quantity;
        updates.push_back(upd);
    }

    for (const auto& lvl : snap.asks)
    {
        provider::l2_update upd;
        upd.timestamp = snap.timestamp;
        upd.symbol = snap.symbol;
        upd.side = 1;
        upd.price = lvl.price;
        upd.new_quantity = lvl.quantity;
        updates.push_back(upd);
    }

    return updates;
}

} // namespace binance

class BinanceDepthSnapshotParser : public IDataParser<provider::l2_snapshot>
{
public:
    bool parse_header(const std::string&) override { return true; }

    std::optional<provider::l2_snapshot> parse_record(const std::string& line) override
    {
        return binance::parse_depth_snapshot(line);
    }
};
