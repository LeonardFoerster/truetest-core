#pragma once

#include "providers/parser.h"
#include "providers/provider_event.h"
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_depth_parser.h"
#include "providers/footprint/decimal_ticks.h"
#include "providers/recovery_payload.h"

#include <cstdlib>
#include <optional>
#include <string>

class BinanceCombinedParser : public IDataParser<provider::event>
{
public:
    bool parse_header(const std::string&) override
    {
        return true;
    }

    // footprint.md §2.1: opt-in exact-decimal wiring. Unset by default (no
    // behavior change - has_exact_decimal stays false on every tick, same
    // as before this method existed). Callers (main.inc, not this class)
    // resolve tick_size themselves - see resolve_footprint_tick_size() -
    // and only call this with a genuinely exact decimal string (the
    // official-metadata path is presently double-only and is NOT routed
    // here to avoid a double round-trip masquerading as "exact"; use
    // --footprint-tick-size for a real exact-decimal source today).
    void configure_exact_decimal(std::string_view tick_size_str, int atom_decimals = 8)
    {
        exact_tick_size_ = truetest::footprint::parse_decimal(tick_size_str);
        atom_decimals_ = atom_decimals;
    }

    std::optional<provider::event> parse_record(const std::string& line) override
    {
        // Accept both combined-stream envelopes ({"stream":...,"data":{...}})
        // and raw single-stream data objects.
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
            t.quantity_scale = rec->quantity_scale;

            // Native trade id ("t") - unconditional, cheap, always exact
            // (it's an integer already). footprint.md §2.1.
            if (const auto id_str = binance::extract_number(data_json, "t"); !id_str.empty())
            {
                char* end = nullptr;
                const auto id = std::strtoull(id_str.c_str(), &end, 10);
                if (end && *end == '\0')
                    t.native_trade_id = id;
            }

            // Exact integer price_ticks/base_qty_atoms - only when the
            // caller configured a genuinely exact tick size (see
            // configure_exact_decimal() above). Parses Binance's raw "p"/"q"
            // decimal strings directly; never touches rec->price/quantity
            // (already lossy doubles) for this path.
            if (exact_tick_size_)
            {
                const auto price_str = binance::extract_number(data_json, "p");
                const auto qty_str = binance::extract_number(data_json, "q");
                const auto price_dec = truetest::footprint::parse_decimal(price_str);
                const auto qty_dec = truetest::footprint::parse_decimal(qty_str);
                if (price_dec && qty_dec)
                {
                    const auto ticks = truetest::footprint::decimal_to_ticks(*price_dec, *exact_tick_size_);
                    const auto atoms = truetest::footprint::decimal_to_atoms(*qty_dec, atom_decimals_);
                    if (ticks && atoms)
                    {
                        t.price_ticks = *ticks;
                        t.base_qty_atoms = *atoms;
                        t.has_exact_decimal = true;
                    }
                }
            }

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
            b.quantity_scale = rec->quantity_scale;
            return provider::event{b};
        }
        else if (event_type == "depthUpdate")
        {
            // A diff frame can contain several level mutations. Returning a
            // snapshot here used to make the engine erase every omitted
            // level. Callers must use parse_records(), which emits all deltas
            // atomically in the frame's venue order.
            return std::nullopt;
        }

        // Partial-book streams (@depth{5|10|20}@...) have no "e"/"s",
        // just {lastUpdateId, bids, asks} - detect by stream-name suffix.
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

    std::vector<provider::event> parse_records(std::string_view line) override
    {
        const std::string stream_name = extract_stream_name(std::string(line));
        std::string data_json = extract_data(std::string(line));
        if (data_json.empty())
            data_json.assign(line.data(), line.size());

        if (binance::extract_string(data_json, "e") == "depthUpdate")
        {
            auto batch = binance::parse_depth_delta_batch(data_json);
            if (!batch) return {};
            return {provider::event{std::move(*batch)}};
        }

        (void)stream_name;
        return IDataParser<provider::event>::parse_records(line);
    }

    empty_parse_status classify_empty_frame(std::string_view line) const override
    {
        // Binance subscription acknowledgements and well-formed x=false
        // kline updates are valid no-data frames. Everything else that
        // produced no market event is malformed/unknown.
        if (!provider_recovery::is_authoritative_object(line)
            || !provider_recovery::decision_members_are_unique(
                line, {"result", "id", "data"}))
            return empty_parse_status::malformed;

        const std::string frame(line);
        std::string data_json = extract_data(frame);
        if (data_json.empty())
            data_json = frame;
        if (is_well_formed_forming_kline(data_json))
            return empty_parse_status::ignored;

        std::string_view result;
        std::string_view id;
        std::string_view data;
        const auto data_state = provider_recovery::payload_parser(line)
            .inspect_top_level_member("data", data);
        std::uint64_t subscription_id = 0;
        if (provider_recovery::top_level_member(line, "result", result)
            && provider_recovery::is_exact_null(result)
            && provider_recovery::top_level_member(line, "id", id)
            && provider_recovery::parse_positive_u64(id, subscription_id)
            && data_state
                == provider_recovery::payload_parser::member_result::missing)
            return empty_parse_status::ignored;
        return empty_parse_status::malformed;
    }

private:
    static bool is_well_formed_forming_kline(const std::string& json)
    {
        if (binance::extract_string(json, "e") != "kline")
            return false;
        const auto closed = binance::extract_sv_optional_bool(json, "x");
        if (!closed.has_value() || *closed)
            return false;

        double value = 0.0;
        for (const std::string_view key : {"o", "h", "l", "c"})
        {
            if (!binance::parse_double_sv(
                    binance::extract_sv_number(json, key), value))
                return false;
        }
        return true;
    }

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

    static std::string extract_stream_name(const std::string& json)
    {
        return binance::extract_string(json, "stream");
    }

    // Match "@depth" + digit so partial-book ("@depth5@…") isn't confused
    // with the diff stream ("@depth@100ms", no level count).
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

    std::optional<truetest::footprint::DecimalValue> exact_tick_size_;
    int atom_decimals_ = 8;
};
