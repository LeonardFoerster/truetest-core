#pragma once

#include "providers/parser.h"
#include "providers/local/csv_parser.h"
#include "providers/provider_event.h"

#include <string>
#include <optional>
#include <chrono>
#include <cstdlib>

// BinanceParser: parses Binance WebSocket JSON messages into records.
//
// Trade messages → tick_record
// Format: {"e":"trade","E":123456789,"s":"BTCUSDT","t":12345,"p":"0.001","q":"100","b":88,"a":50,"T":123456785,"m":true,"M":true}
//
// Kline messages → bar_record
// Format: {"e":"kline","E":123456789,"s":"BTCUSDT","k":{"t":123400000,"T":123459999,"s":"BTCUSDT","i":"1m","o":"0.0010","c":"0.0020","h":"0.0025","l":"0.0015","v":"1000","n":100,...}}

namespace binance {

// Simple JSON field extractor — avoids dependency on a JSON library.
// Extracts the string value for a given key from a JSON object.
inline std::string extract_string(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// Extracts a numeric value (as string) for a given key.
inline std::string extract_number(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();

    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        pos++;

    // If it's a quoted number, extract between quotes
    if (pos < json.size() && json[pos] == '"')
    {
        pos++;
        auto end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    }

    // Otherwise extract until comma, brace, or bracket
    auto end = json.find_first_of(",}]", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// Extracts a boolean value for a given key.
inline bool extract_bool(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos += search.size();
    while (pos < json.size() && json[pos] == ' ') pos++;
    return pos < json.size() && json[pos] == 't';
}

// Parse a Binance trade message into a tick_record.
inline std::optional<tick_record> parse_trade(const std::string& json)
{
    auto event_type = extract_string(json, "e");
    if (event_type != "trade") return std::nullopt;

    auto price_str = extract_string(json, "p");
    auto qty_str = extract_string(json, "q");
    auto symbol = extract_string(json, "s");
    auto time_str = extract_number(json, "T");  // Trade time

    if (price_str.empty() || qty_str.empty() || symbol.empty())
        return std::nullopt;

    tick_record rec;
    rec.price = std::stod(price_str);
    rec.quantity = static_cast<int64_t>(std::stod(qty_str) * 1e8);  // scale to satoshis
    rec.symbol = symbol;

    // "m" field: true = buyer is maker (i.e. sell aggressor), false = buy aggressor
    bool buyer_is_maker = extract_bool(json, "m");
    rec.side = buyer_is_maker ? data_tick_side::ask : data_tick_side::bid;

    // Timestamp
    if (!time_str.empty())
    {
        int64_t ts_ms = std::stoll(time_str);
        rec.timestamp = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(ts_ms));
    }
    else
    {
        rec.timestamp = std::chrono::system_clock::now();
    }

    return rec;
}

// Parse a Binance kline message into a bar_record.
inline std::optional<bar_record> parse_kline(const std::string& json)
{
    auto event_type = extract_string(json, "e");
    if (event_type != "kline") return std::nullopt;

    // Kline data is nested inside "k" object
    auto k_pos = json.find("\"k\":{");
    if (k_pos == std::string::npos) return std::nullopt;
    auto k_json = json.substr(k_pos);

    auto open = extract_string(k_json, "o");
    auto close = extract_string(k_json, "c");
    auto high = extract_string(k_json, "h");
    auto low = extract_string(k_json, "l");
    auto volume = extract_string(k_json, "v");
    auto symbol = extract_string(k_json, "s");

    if (open.empty() || close.empty() || high.empty() || low.empty())
        return std::nullopt;

    bar_record rec;
    rec.symbol = symbol;
    rec.open = std::stod(open);
    rec.high = std::stod(high);
    rec.low = std::stod(low);
    rec.close = std::stod(close);
    rec.volume = volume.empty() ? 0 : static_cast<int64_t>(std::stod(volume) * 1e8);

    // Use kline close time as the date
    auto time_str = extract_number(k_json, "t");
    rec.date = time_str.empty() ? "" : time_str;

    return rec;
}

} // namespace binance

// BinanceTradeParser: IDataParser adapter for Binance trade messages.
// Each JSON line from the WebSocket is parsed into a tick_record.
class BinanceTradeParser : public IDataParser<tick_record>
{
public:
    bool parse_header(const std::string&) override
    {
        // Binance WebSocket has no header line
        return true;
    }

    std::optional<tick_record> parse_record(const std::string& line) override
    {
        return binance::parse_trade(line);
    }
};

// BinanceKlineParser: IDataParser adapter for Binance kline messages.
class BinanceKlineParser : public IDataParser<bar_record>
{
public:
    bool parse_header(const std::string&) override
    {
        return true;
    }

    std::optional<bar_record> parse_record(const std::string& line) override
    {
        return binance::parse_kline(line);
    }
};

