#pragma once
#ifdef HAS_BITUNIX

// Hand-rolled Bitunix public WS parsers (no nlohmann on this path).
// Trade push shape:
//   {"ch":"trade","symbol":"BTCUSDT","ts":...,"data":[{"p":"...","v":"...","s":"buy","t":"..."},...]}

#include "providers/parser.h"
#include "providers/provider_event.h"
#include "providers/local/csv_parser.h"
#include "providers/recovery_payload.h"
#include "data/date_parse.h"
#include "data/quantity_scale.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bitunix {

namespace detail {

inline std::optional<double> parse_double_token(std::string_view tok)
{
    if (tok.empty())
        return std::nullopt;
    double value = 0.0;
    const auto [end, error] =
        std::from_chars(tok.data(), tok.data() + tok.size(), value);
    if (error != std::errc{} || end != tok.data() + tok.size()
        || !std::isfinite(value))
        return std::nullopt;
    return value;
}

} // namespace detail

inline std::string extract_channel(std::string_view json)
{
    std::string_view channel;
    if (provider_recovery::top_level_plain_string(json, "ch", channel))
        return std::string(channel);
    return {};
}

struct occurrence_time_range
{
    std::chrono::system_clock::time_point first;
    std::chrono::system_clock::time_point last;
};

// Parse all trades in a public trade push into tick_records. Records carry
// known-at time; the optional range preserves venue occurrence time for the
// stateful cross-frame late/replay gate.
inline std::vector<tick_record> parse_all_trades(
    std::string_view json, occurrence_time_range* occurrence_range = nullptr)
{
    if (occurrence_range) *occurrence_range = {};
    std::vector<tick_record> out;
    if (!provider_recovery::is_authoritative_object(json))
        return out;

    std::string_view channel;
    std::string_view symbol;
    std::string_view data;
    if (!provider_recovery::top_level_plain_string(json, "ch", channel)
        || channel != "trade"
        || !provider_recovery::top_level_plain_string(json, "symbol", symbol)
        || symbol.empty()
        || !provider_recovery::top_level_member(json, "data", data)
        || !provider_recovery::is_authoritative_object_array(data))
        return out;

    std::optional<std::chrono::system_clock::time_point> frame_time;
    std::string_view raw_frame_time;
    const auto frame_time_result = provider_recovery::payload_parser(json)
        .inspect_top_level_member("ts", raw_frame_time);
    if (frame_time_result
        != provider_recovery::payload_parser::member_result::unique)
        return out;
    std::string_view frame_time_text;
    if (!provider_recovery::top_level_scalar_text(
            json, "ts", frame_time_text))
        return out;
    std::int64_t frame_time_ms = 0;
    const auto [end, error] = std::from_chars(
        frame_time_text.data(),
        frame_time_text.data() + frame_time_text.size(), frame_time_ms);
    if (error != std::errc{}
        || end != frame_time_text.data() + frame_time_text.size())
        return out;
    frame_time = tt::date_parse::from_epoch_milliseconds(frame_time_ms);
    if (!frame_time)
        return out;

    bool invalid_element = false;
    std::optional<std::chrono::system_clock::time_point> first_event_time;
    std::optional<std::chrono::system_clock::time_point> last_event_time;
    const bool valid_array = provider_recovery::every_top_level_object(
        data, [&](std::string_view obj) {
        std::string_view price_text;
        std::string_view quantity_text;
        std::string_view side;
        if (!provider_recovery::top_level_scalar_text(obj, "p", price_text)
            || !provider_recovery::top_level_scalar_text(
                obj, "v", quantity_text)
            || !provider_recovery::top_level_plain_string(obj, "s", side))
        {
            invalid_element = true;
            return false;
        }
        const auto price = detail::parse_double_token(price_text);
        const auto qty_atoms =
            tt::quantity_scale::parse_decimal_canonical_atoms(quantity_text);
        if (!price || !(*price > 0.0) || !qty_atoms || *qty_atoms <= 0
            || (side != "buy" && side != "BUY"
                && side != "sell" && side != "SELL"))
        {
            invalid_element = true;
            return false;
        }

        std::optional<std::chrono::system_clock::time_point> event_time;
        std::string_view raw_event_time;
        const auto event_time_result = provider_recovery::payload_parser(obj)
            .inspect_top_level_member("t", raw_event_time);
        if (event_time_result
            == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
        {
            invalid_element = true;
            return false;
        }
        if (event_time_result
            == provider_recovery::payload_parser::member_result::unique)
        {
            std::string_view event_time_text;
            if (!provider_recovery::top_level_plain_string(
                    obj, "t", event_time_text))
            {
                invalid_element = true;
                return false;
            }
            event_time = tt::date_parse::parse(event_time_text);
        }
        else
        {
            event_time = frame_time;
        }
        if (!event_time)
        {
            invalid_element = true;
            return false;
        }
        if ((last_event_time && *event_time < *last_event_time)
            || (frame_time && *event_time > *frame_time))
        {
            invalid_element = true;
            return false;
        }
        if (!first_event_time) first_event_time = event_time;
        last_event_time = event_time;

        tick_record rec;
        rec.price = *price;
        // Domain Tick uses fixed-scale int64 qty (1e8), same as Binance/Bitget.
        rec.quantity = *qty_atoms;
        rec.quantity_scale = tt::quantity_scale::canonical_atoms;
        rec.symbol.assign(symbol.data(), symbol.size());
        rec.side = (side == "buy" || side == "BUY")
            ? data_tick_side::bid : data_tick_side::ask;
        // The engine's single timestamp is decision/known-at time. Preserve
        // the row timestamp as a causality check, but never expose the trade
        // to a strategy before the containing frame timestamp.
        rec.timestamp = *frame_time;
        out.push_back(std::move(rec));
        return true;
    });
    if (!valid_array || invalid_element) {
        out.clear();
    } else if (!out.empty() && occurrence_range) {
        occurrence_range->first = *first_event_time;
        occurrence_range->last = *last_event_time;
    }
    return out;
}

inline std::optional<tick_record> parse_trade_first(std::string_view json)
{
    auto all = parse_all_trades(json);
    if (all.empty())
        return std::nullopt;
    return all.front();
}

inline provider::tick to_provider_tick(const tick_record& rec)
{
    provider::tick t;
    t.timestamp = rec.timestamp;
    t.symbol = rec.symbol;
    t.price = rec.price;
    t.quantity = rec.quantity;
    t.quantity_scale = rec.quantity_scale;
    t.side = (rec.side == data_tick_side::bid) ? 0
           : (rec.side == data_tick_side::ask) ? 1 : 2;
    return t;
}

} // namespace bitunix

// ---------------------------------------------------------------------------
// IDataParser adapters
// ---------------------------------------------------------------------------

class BitunixTradeParser : public IDataParser<tick_record>
{
public:
    std::optional<tick_record> parse_record(const std::string& line) override
    {
        return parse_record(std::string_view{line});
    }

    std::optional<tick_record> parse_record(std::string_view line) override
    {
        auto batch = parse_records(line);
        if (batch.empty()) return std::nullopt;
        return std::move(batch.front());
    }

    std::vector<tick_record> parse_records(std::string_view line) override
    {
        bitunix::occurrence_time_range occurrence;
        auto batch = bitunix::parse_all_trades(line, &occurrence);
        if (batch.empty()) return batch;
        const auto known_at = batch.front().timestamp;
        if (last_occurrence_timestamp_
            && occurrence.first < *last_occurrence_timestamp_)
            return {};
        if (last_known_at_timestamp_ && known_at < *last_known_at_timestamp_)
            return {};
        last_occurrence_timestamp_ = occurrence.last;
        last_known_at_timestamp_ = known_at;
        return batch;
    }

private:
    std::optional<std::chrono::system_clock::time_point>
        last_occurrence_timestamp_;
    std::optional<std::chrono::system_clock::time_point>
        last_known_at_timestamp_;
};

class BitunixCombinedParser : public IDataParser<provider::event>
{
public:
    std::optional<provider::event> parse_record(const std::string& line) override
    {
        return parse_record(std::string_view{line});
    }

    std::optional<provider::event> parse_record(std::string_view line) override
    {
        auto batch = parse_records(line);
        if (batch.empty())
            return std::nullopt;
        return batch.front();
    }

    std::vector<provider::event> parse_records(std::string_view line) override
    {
        std::vector<provider::event> out;
        if (classify_empty_frame(line) == empty_parse_status::ignored)
            return out;

        bitunix::occurrence_time_range occurrence;
        auto records = bitunix::parse_all_trades(line, &occurrence);
        const auto known_at = records.empty()
            ? std::chrono::system_clock::time_point{}
            : records.front().timestamp;
        if (!records.empty()
            && last_occurrence_timestamp_
            && occurrence.first < *last_occurrence_timestamp_)
            return out;
        if (!records.empty() && last_known_at_timestamp_
            && known_at < *last_known_at_timestamp_)
            return out;
        if (!records.empty())
        {
            last_occurrence_timestamp_ = occurrence.last;
            last_known_at_timestamp_ = known_at;
        }
        for (auto& rec : records)
            out.emplace_back(bitunix::to_provider_tick(rec));
        return out;
    }

    empty_parse_status classify_empty_frame(std::string_view line) const override
    {
        if (!provider_recovery::is_authoritative_object(line)
            || !provider_recovery::decision_members_are_unique(
                line, {"ch", "op", "data"}))
            return empty_parse_status::malformed;
        const auto ch = bitunix::extract_channel(line);
        std::string_view data;
        const bool no_data = provider_recovery::payload_parser(line)
            .inspect_top_level_member("data", data)
            == provider_recovery::payload_parser::member_result::missing;
        std::string_view op;
        const bool has_op = provider_recovery::top_level_plain_string(
            line, "op", op);
        if (!ch.empty() && has_op)
            return empty_parse_status::malformed;
        if (!ch.empty() && ch != "trade")
            return empty_parse_status::ignored;
        if (no_data && has_op && op == "pong")
            return empty_parse_status::ignored;
        if (no_data && has_op && op == "subscribe") {
            std::string_view code;
            if (provider_recovery::top_level_scalar_text(line, "code", code)
                && provider_recovery::trim_json_ws(code) == "0")
                return empty_parse_status::ignored;
        }
        return empty_parse_status::malformed;
    }

private:
    std::optional<std::chrono::system_clock::time_point>
        last_occurrence_timestamp_;
    std::optional<std::chrono::system_clock::time_point>
        last_known_at_timestamp_;
};

#endif // HAS_BITUNIX
