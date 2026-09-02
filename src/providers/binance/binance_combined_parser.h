#pragma once

#include "providers/parser.h"
#include "providers/provider_event.h"
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_depth_parser.h"
#include "providers/footprint/decimal_ticks.h"
#include "providers/recovery_payload.h"

#include <charconv>
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
        std::string_view stream_name;
        std::string_view data_json;
        if (!authoritative_market_payload(line, stream_name, data_json))
            return std::nullopt;

        std::string_view event_type;
        (void)provider_recovery::top_level_plain_string(
            data_json, "e", event_type);
        if (!stream_matches_payload(stream_name, event_type, data_json))
            return std::nullopt;

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
            if (last_timestamp_ && t.timestamp < *last_timestamp_)
                return std::nullopt;

            // Native trade id ("t") - unconditional, cheap, always exact
            // (it's an integer already). footprint.md §2.1.
            std::string_view id_text;
            if (provider_recovery::top_level_scalar_text(
                    data_json, "t", id_text))
            {
                std::uint64_t id = 0;
                const auto [end, error] = std::from_chars(
                    id_text.data(), id_text.data() + id_text.size(), id);
                if (error == std::errc{}
                    && end == id_text.data() + id_text.size())
                    t.native_trade_id = id;
            }

            // Exact integer price_ticks/base_qty_atoms - only when the
            // caller configured a genuinely exact tick size (see
            // configure_exact_decimal() above). Parses Binance's raw "p"/"q"
            // decimal strings directly; never touches rec->price/quantity
            // (already lossy doubles) for this path.
            if (exact_tick_size_)
            {
                std::string_view price_str;
                std::string_view qty_str;
                (void)provider_recovery::top_level_plain_string(
                    data_json, "p", price_str);
                (void)provider_recovery::top_level_plain_string(
                    data_json, "q", qty_str);
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

            last_timestamp_ = t.timestamp;
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
            std::int64_t known_ms = 0;
            if (!binance::parse_int64_sv(rec->date, known_ms))
                return std::nullopt;
            const auto known_time =
                tt::date_parse::from_epoch_milliseconds(known_ms);
            if (!known_time
                || (last_timestamp_ && *known_time < *last_timestamp_))
                return std::nullopt;
            last_timestamp_ = *known_time;
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
        std::string_view stream_name;
        std::string_view data_json;
        if (!authoritative_market_payload(line, stream_name, data_json))
            return {};

        std::string_view event_type;
        if (provider_recovery::top_level_plain_string(
                data_json, "e", event_type)
            && event_type == "depthUpdate")
        {
            if (!stream_matches_payload(stream_name, event_type, data_json))
                return {};
            auto batch = binance::parse_depth_delta_batch(data_json);
            if (!batch) return {};
            if (last_timestamp_ && batch->timestamp < *last_timestamp_)
                return {};
            last_timestamp_ = batch->timestamp;
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

        std::string_view stream_name;
        std::string_view data_json;
        std::string_view event_type;
        if (authoritative_market_payload(line, stream_name, data_json)
            && provider_recovery::top_level_plain_string(
                data_json, "e", event_type)
            && stream_matches_payload(stream_name, event_type, data_json)
            && is_well_formed_forming_kline(data_json))
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
    static bool is_well_formed_forming_kline(std::string_view json)
    {
        return binance::is_well_formed_forming_kline(json);
    }

    static bool authoritative_market_payload(std::string_view json,
                                             std::string_view& stream_name,
                                             std::string_view& data_json)
    {
        stream_name = {};
        data_json = {};
        if (!provider_recovery::is_authoritative_object(json)
            || !provider_recovery::decision_members_are_unique(
                json, {"stream", "data"}))
            return false;

        std::string_view data;
        const auto data_state = provider_recovery::payload_parser(json)
            .inspect_top_level_member("data", data);
        if (data_state
            == provider_recovery::payload_parser::member_result::unique)
        {
            data = provider_recovery::trim_json_ws(data);
            if (!provider_recovery::top_level_plain_string(
                    json, "stream", stream_name)
                || stream_name.empty()
                || !provider_recovery::is_authoritative_object(data))
                return false;
            data_json = data;
            return true;
        }
        if (data_state
            == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
            return false;

        std::string_view unexpected_stream;
        if (provider_recovery::top_level_plain_string(
                json, "stream", unexpected_stream))
            return false;
        data_json = json;
        return true;
    }

    // Match "@depth" + digit so partial-book ("@depth5@…") isn't confused
    // with the diff stream ("@depth@100ms", no level count).
    static bool is_partial_book_stream(std::string_view stream_name)
    {
        auto pos = stream_name.find("@depth");
        if (pos == std::string::npos) return false;
        const auto after = pos + 6;
        return after < stream_name.size() &&
               stream_name[after] >= '0' && stream_name[after] <= '9';
    }

    static bool stream_matches_payload(std::string_view stream_name,
                                       std::string_view event_type,
                                       std::string_view payload)
    {
        if (stream_name.empty()) return true;
        const auto at = stream_name.find('@');
        if (at == std::string_view::npos || at == 0) return false;

        std::string expected_symbol{stream_name.substr(0, at)};
        for (auto& c : expected_symbol)
            c = static_cast<char>(std::toupper(
                static_cast<unsigned char>(c)));
        std::string_view payload_symbol;
        if (provider_recovery::top_level_plain_string(
                payload, "s", payload_symbol)
            && payload_symbol != expected_symbol)
            return false;

        const auto channel = stream_name.substr(at);
        if (channel == "@trade") return event_type == "trade";
        if (channel.rfind("@kline_", 0) == 0)
        {
            if (event_type != "kline") return false;
            std::string_view kline;
            std::string_view interval;
            return provider_recovery::top_level_member(payload, "k", kline)
                && provider_recovery::is_authoritative_object(kline)
                && provider_recovery::top_level_plain_string(
                    kline, "i", interval)
                && !interval.empty()
                && channel.substr(std::string_view{"@kline_"}.size())
                    == interval;
        }
        if (channel.rfind("@depth@", 0) == 0)
            return event_type == "depthUpdate";
        if (is_partial_book_stream(stream_name))
            return event_type.empty() && payload_symbol.empty();
        return false;
    }

    static std::string symbol_from_stream(std::string_view stream_name)
    {
        auto at = stream_name.find('@');
        std::string sym{(at == std::string_view::npos)
            ? stream_name
            : stream_name.substr(0, at)};
        for (auto& c : sym)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return sym;
    }

    std::optional<truetest::footprint::DecimalValue> exact_tick_size_;
    std::optional<std::chrono::system_clock::time_point> last_timestamp_;
    int atom_decimals_ = 8;
};
