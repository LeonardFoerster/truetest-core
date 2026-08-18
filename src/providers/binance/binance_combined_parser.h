#pragma once

#include "providers/parser.h"
#include "providers/provider_event.h"
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_depth_parser.h"
#include "providers/footprint/decimal_ticks.h"
#include "providers/recovery_payload.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

class BinanceCombinedParser : public IDataParser<provider::event>
{
public:
    // Legacy/replay callers retain the old projection for compatibility.
    // Live providers choose refuse_raw_diff_depth: raw `depthUpdate` needs a
    // separate sequence synchronizer and must never replace the local book.
    enum class depth_update_policy : std::uint8_t
    {
        legacy_snapshot_compatibility,
        refuse_raw_diff_depth,
    };

    explicit BinanceCombinedParser(
        depth_update_policy depth_policy =
            depth_update_policy::legacy_snapshot_compatibility) noexcept
        : depth_policy_(depth_policy)
    {}

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
        // A live partial-book record must carry its combined-stream identity.
        // Handle that branch before generic event dispatch: a frame labelled
        // `@depth20` must never be reinterpreted as an unrelated trade/kline
        // merely because its body was malformed or injected.
        const std::string stream_name = extract_stream_name(line);
        const bool depth_like_frame =
            std::string_view(line.data(), line.size()).find("@depth")
                != std::string_view::npos;
        if (depth_policy_ == depth_update_policy::refuse_raw_diff_depth
            && (is_partial_book_stream(stream_name) || depth_like_frame))
        {
            auto snap = parse_live_partial_book_frame(line);
            if (!snap) return std::nullopt;
            return provider::event{std::move(*snap)};
        }

        // Accept both combined-stream envelopes ({"stream":...,"data":{...}})
        // and raw single-stream data objects for legacy/replay callers.
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
            return provider::event{b};
        }
        else if (event_type == "depthUpdate")
        {
            if (depth_policy_ == depth_update_policy::refuse_raw_diff_depth)
                return std::nullopt;
            auto snap = binance::parse_depth_snapshot(data_json);
            if (!snap) return std::nullopt;
            return provider::event{*snap};
        }

        // Partial-book streams (@depth{5|10|20}@...) have no "e"/"s",
        // just {lastUpdateId, bids, asks} - detect by stream-name suffix.
        if (is_partial_book_stream(stream_name))
        {
            if (depth_policy_ == depth_update_policy::refuse_raw_diff_depth)
            {
                // The live branch above is intentionally the only admission
                // path.  In particular, never fall back to the permissive
                // parser when strict envelope or level validation failed.
                return std::nullopt;
            }
            auto snap = binance::parse_depth_snapshot(data_json);
            if (!snap) return std::nullopt;
            if (snap->symbol.empty())
                snap->symbol = symbol_from_stream(stream_name);
            return provider::event{*snap};
        }

        return std::nullopt;
    }

    empty_parse_status classify_empty_frame(std::string_view line) const override
    {
        // Binance subscription acknowledgements have a null result and an id.
        // Everything else that produced no market event is malformed/unknown.
        if (!provider_recovery::is_authoritative_object(line)
            || !provider_recovery::decision_members_are_unique(
                line, {"result", "id", "data"}))
            return empty_parse_status::malformed;
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

    // The stream name contains the symbol before its first '@'; classify only
    // the suffix so a raw diff stream can never masquerade as a partial book.
    static bool is_partial_book_stream(const std::string& stream_name)
    {
        const auto at = stream_name.find('@');
        return at != std::string::npos
            && binance::is_explicit_partial_book_depth_stream(
                std::string_view(stream_name).substr(at + 1));
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

    static std::optional<std::string> strict_partial_stream_symbol(
        std::string_view stream_name)
    {
        const auto at = stream_name.find('@');
        if (at == std::string_view::npos || at == 0
            || at > binance::depth_detail::max_live_partial_book_symbol_bytes)
            return std::nullopt;

        const std::string_view symbol = stream_name.substr(0, at);
        const std::string_view suffix = stream_name.substr(at + 1);
        if (binance::explicit_partial_book_depth_level_limit(suffix) == 0
            || !binance::depth_detail::is_ascii_binance_symbol(symbol))
            return std::nullopt;

        std::string canonical_symbol;
        canonical_symbol.reserve(symbol.size());
        for (const unsigned char c : symbol)
        {
            canonical_symbol.push_back(static_cast<char>(
                (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c));
        }
        return canonical_symbol;
    }

    static std::optional<provider::l2_snapshot> parse_live_partial_book_frame(
        std::string_view frame)
    {
        // The strict payload parser imposes its own body bound.  The small
        // envelope allowance rejects oversized wrappers before repeated full
        // document scans while leaving ample room for Binance's stream name.
        constexpr std::size_t max_frame_bytes =
            binance::depth_detail::max_live_partial_book_payload_bytes + 512;
        if (frame.empty() || frame.size() > max_frame_bytes
            || !provider_recovery::is_authoritative_object(frame)
            || !provider_recovery::decision_members_are_unique(
                frame, {"stream", "data"}))
            return std::nullopt;

        std::string_view stream_name;
        std::string_view data;
        if (!provider_recovery::top_level_plain_string(
                frame, "stream", stream_name)
            || !provider_recovery::top_level_member(frame, "data", data))
            return std::nullopt;

        auto canonical_symbol = strict_partial_stream_symbol(stream_name);
        if (!canonical_symbol) return std::nullopt;

        const auto at = stream_name.find('@');
        const auto level_limit = binance::explicit_partial_book_depth_level_limit(
            stream_name.substr(at + 1));
        return binance::parse_strict_partial_book_snapshot(
            data, *canonical_symbol, level_limit);
    }

    std::optional<truetest::footprint::DecimalValue> exact_tick_size_;
    int atom_decimals_ = 8;
    depth_update_policy depth_policy_;
};
