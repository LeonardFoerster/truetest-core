#pragma once

#include "providers/parser.h"
#include "providers/provider_event.h"
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_depth_parser.h"

#include <optional>
#include <string>

class BinanceCombinedParser : public IDataParser<provider::event>
{
public:
    bool parse_header(const std::string&) override
    {
        return true;
    }

    std::optional<provider::event> parse_record(const std::string& line) override
    {
        // Combined-stream frames are wrapped in {"stream":"...","data":{...}}.
        // Single-stream frames arrive as the raw data object — accept both so
        // the same parser works whether BinanceProvider subscribed to one
        // stream or several.
        const std::string stream_name = extract_stream_name(line);
        std::string data_json = extract_data(line);
        if (data_json.empty())
        {
            data_json = line;
        }

        auto event_type = binance::extract_string(data_json, "e");

        if (event_type == "trade")
        {
            auto rec = binance::parse_trade(data_json);
            if (!rec) return std::nullopt;

            provider::tick t;
            t.timestamp = rec->timestamp;
            t.symbol = rec->symbol;
            t.price = rec->price;
            t.quantity = rec->quantity;
            t.side = (rec->side == data_tick_side::bid) ? 0 :
                     (rec->side == data_tick_side::ask) ? 1 : 2;
            return provider::event{t};
        }
        else if (event_type == "kline")
        {
            auto rec = binance::parse_kline(data_json);
            if (!rec) return std::nullopt;

            provider::bar b;
            b.date = rec->date;
            b.symbol = rec->symbol;
            b.open = rec->open;
            b.high = rec->high;
            b.low = rec->low;
            b.close = rec->close;
            b.volume = rec->volume;
            return provider::event{b};
        }
        else if (event_type == "depthUpdate")
        {
            auto snap = binance::parse_depth_snapshot(data_json);
            if (!snap) return std::nullopt;
            return provider::event{*snap};
        }

        // Partial-book streams (@depth5@100ms / @depth10 / @depth20) send
        // frames with no "e" / "s" fields — just {"lastUpdateId", "bids",
        // "asks"}. Detect them by the stream-name suffix in the envelope.
        if (is_partial_book_stream(stream_name))
        {
            auto snap = binance::parse_depth_snapshot(data_json);
            if (!snap) return std::nullopt;
            if (snap->symbol.empty())
                snap->symbol = symbol_from_stream(stream_name);
            return provider::event{*snap};
        }

        return std::nullopt;
    }

private:
    static std::string extract_data(const std::string& json)
    {
        std::string search = "\"data\":";
        auto pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();

        while (pos < json.size() && json[pos] == ' ') pos++;

        if (pos >= json.size() || json[pos] != '{') return "";

        int depth = 0;
        size_t start = pos;
        for (size_t i = pos; i < json.size(); ++i)
        {
            if (json[i] == '{') depth++;
            else if (json[i] == '}') depth--;
            if (depth == 0)
                return json.substr(start, i - start + 1);
        }

        return "";
    }

    // Pull the "stream":"..." value out of a combined-stream envelope.
    // Returns empty when the frame is a raw (single-stream) data object.
    static std::string extract_stream_name(const std::string& json)
    {
        return binance::extract_string(json, "stream");
    }

    // Partial-book streams have the form <symbol>@depth{5|10|20}@<ms>ms.
    // We match the "@depth" marker followed by a digit to avoid confusing
    // them with the diff stream "@depth@100ms" (which has no level count).
    static bool is_partial_book_stream(const std::string& stream_name)
    {
        auto pos = stream_name.find("@depth");
        if (pos == std::string::npos) return false;
        const auto after = pos + 6;
        return after < stream_name.size() &&
               stream_name[after] >= '0' && stream_name[after] <= '9';
    }

    static std::string symbol_from_stream(const std::string& stream_name)
    {
        auto at = stream_name.find('@');
        std::string sym = (at == std::string::npos)
            ? stream_name
            : stream_name.substr(0, at);
        for (auto& c : sym)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return sym;
    }
};
