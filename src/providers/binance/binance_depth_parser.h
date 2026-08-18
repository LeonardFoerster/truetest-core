#pragma once

#include "providers/parser.h"
#include "providers/provider_event.h"
#include "providers/binance/binance_parser.h"
#include "providers/recovery_payload.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace binance {

// The Binance raw `depthUpdate` channel is a sequenced delta stream, not a
// self-contained book.  The live providers deliberately support only these
// explicit partial-book contracts until a sequence synchronizer is added:
//
//   depth{5|10|20}[@100ms|@1000ms]
//
// Keep this classifier next to the depth parser so provider admission and the
// combined-stream parser share exactly the same definition of a safe snapshot
// source.  Unknown spellings are intentionally not treated as snapshots.
inline std::size_t explicit_partial_book_depth_level_limit(
    std::string_view stream_suffix) noexcept
{
    constexpr std::string_view prefix{"depth"};
    if (!stream_suffix.starts_with(prefix)) return 0;
    stream_suffix.remove_prefix(prefix.size());

    std::string_view cadence;
    std::size_t levels = 0;
    if (stream_suffix.starts_with("5"))
    {
        cadence = stream_suffix.substr(1);
        levels = 5;
    }
    else if (stream_suffix.starts_with("10"))
    {
        cadence = stream_suffix.substr(2);
        levels = 10;
    }
    else if (stream_suffix.starts_with("20"))
    {
        cadence = stream_suffix.substr(2);
        levels = 20;
    }
    else
        return 0;

    return (cadence.empty() || cadence == "@100ms" || cadence == "@1000ms")
        ? levels
        : 0;
}

inline bool is_explicit_partial_book_depth_stream(
    std::string_view stream_suffix) noexcept
{
    return explicit_partial_book_depth_level_limit(stream_suffix) != 0;
}

namespace depth_detail {

// Locate `"key":[` (no whitespace variants; Binance never spaces here).
// Returns index of first element after `[`, or npos.
inline std::size_t find_level_array(std::string_view json, std::string_view key)
{
    // Build needle without heap: "key":[
    // Max key length we use is 4 ("bids"/"asks").
    char needle[16];
    if (key.size() + 4 > sizeof(needle))
        return std::string_view::npos;
    needle[0] = '"';
    for (std::size_t i = 0; i < key.size(); ++i)
        needle[1 + i] = key[i];
    needle[1 + key.size()] = '"';
    needle[2 + key.size()] = ':';
    needle[3 + key.size()] = '[';
    const std::size_t nlen = 4 + key.size();

    auto pos = json.find(std::string_view(needle, nlen));
    if (pos == std::string_view::npos)
        return std::string_view::npos;
    return pos + nlen;
}

// Parse one Binance depth level: ["price","qty"] starting at pos (on '[').
// Advances pos past the closing ']'. Returns false on malformed input.
inline bool parse_one_level(std::string_view json, std::size_t& pos,
                            double& price, double& qty)
{
    const std::size_t n = json.size();
    while (pos < n && (json[pos] == ' ' || json[pos] == ','))
        ++pos;
    if (pos >= n || json[pos] == ']')
        return false;
    if (json[pos] != '[')
        return false;
    ++pos;

    // price as "..."
    while (pos < n && json[pos] == ' ') ++pos;
    if (pos >= n || json[pos] != '"') return false;
    ++pos;
    auto price_end = json.find('"', pos);
    if (price_end == std::string_view::npos) return false;
    std::string_view price_sv = json.substr(pos, price_end - pos);
    pos = price_end + 1;

    while (pos < n && (json[pos] == ',' || json[pos] == ' ')) ++pos;

    // qty as "..."
    if (pos >= n || json[pos] != '"') return false;
    ++pos;
    auto qty_end = json.find('"', pos);
    if (qty_end == std::string_view::npos) return false;
    std::string_view qty_sv = json.substr(pos, qty_end - pos);
    pos = qty_end + 1;

    while (pos < n && json[pos] != ']') ++pos;
    if (pos < n) ++pos; // skip ']'

    if (!parse_double_sv(price_sv, price)) return false;
    if (!parse_double_sv(qty_sv, qty)) return false;
    return true;
}

// Append levels from `"key":[ ... ]` into out. Uses from_chars (no stod/substr allocs).
inline void append_levels(std::string_view json, std::string_view key,
                          std::vector<provider::l2_snapshot::level>& out)
{
    auto pos = find_level_array(json, key);
    if (pos == std::string_view::npos) return;

    // Typical partial book is depth5/10/20 — reserve once.
    if (out.capacity() < out.size() + 20)
        out.reserve(out.size() + 20);

    const std::size_t n = json.size();
    while (pos < n)
    {
        double price = 0.0, qty = 0.0;
        std::size_t save = pos;
        if (!parse_one_level(json, pos, price, qty))
        {
            // End of array or malformed — stop cleanly on ']'.
            while (pos < n && (json[pos] == ' ' || json[pos] == ',')) ++pos;
            break;
        }
        (void)save;
        out.push_back({price, static_cast<int64_t>(qty * 1e8)});

        while (pos < n && (json[pos] == ' ' || json[pos] == ',')) ++pos;
        if (pos >= n || json[pos] == ']')
            break;
    }
}

// Prefer long keys ("bids"/"asks"), fall back to short ("b"/"a").
inline void parse_side_levels(std::string_view json,
                              std::string_view long_key, std::string_view short_key,
                              std::vector<provider::l2_snapshot::level>& out)
{
    out.clear();
    append_levels(json, long_key, out);
    if (out.empty())
        append_levels(json, short_key, out);
}

inline std::chrono::system_clock::time_point parse_event_time(std::string_view json)
{
    auto ts_sv = extract_sv_number(json, "E");
    int64_t ts_ms = 0;
    if (parse_int64_sv(ts_sv, ts_ms))
    {
        return std::chrono::system_clock::time_point(
            std::chrono::milliseconds(ts_ms));
    }
    return std::chrono::system_clock::now();
}

// A partial-book frame is admitted to a live engine only after full document
// validation.  The legacy helpers above deliberately remain permissive for
// replay compatibility, so keep this parser separate rather than making
// those callers unexpectedly reject historical fixtures.
constexpr std::size_t max_live_partial_book_payload_bytes = 16 * 1024;
constexpr std::size_t max_live_partial_book_symbol_bytes = 32;
constexpr std::size_t max_live_partial_book_decimal_bytes = 64;

inline void skip_json_ws(std::string_view input, std::size_t& pos) noexcept
{
    while (pos < input.size()
           && (input[pos] == ' ' || input[pos] == '\t'
               || input[pos] == '\n' || input[pos] == '\r'))
        ++pos;
}

// Decimal strings in the Binance book contract are plain non-negative base
// ten literals.  Reject exponent, sign, escape, and non-ASCII spellings so
// the checked conversion below cannot reinterpret a malformed JSON string.
inline bool parse_plain_decimal(std::string_view input, double& out) noexcept
{
    if (input.empty() || input.size() > max_live_partial_book_decimal_bytes)
        return false;

    bool saw_dot = false;
    std::size_t whole_digits = 0;
    std::size_t fractional_digits = 0;
    for (const char c : input)
    {
        if (c >= '0' && c <= '9')
        {
            if (saw_dot) ++fractional_digits;
            else ++whole_digits;
            continue;
        }
        if (c == '.' && !saw_dot)
        {
            saw_dot = true;
            continue;
        }
        return false;
    }
    if (whole_digits == 0 || (saw_dot && fractional_digits == 0))
        return false;

    return parse_double_sv(input, out);
}

inline bool parse_decimal_json_string(std::string_view input,
                                      std::size_t& pos,
                                      std::string_view& out) noexcept
{
    skip_json_ws(input, pos);
    if (pos >= input.size() || input[pos] != '"') return false;

    const std::size_t begin = ++pos;
    while (pos < input.size())
    {
        const unsigned char c = static_cast<unsigned char>(input[pos++]);
        if (c == '"')
        {
            out = input.substr(begin, pos - begin - 1);
            return true;
        }
        // JSON escapes would make the raw numeric spelling ambiguous.  The
        // venue does not emit them for price/quantity fields.
        if (c < 0x20 || c == '\\') return false;
    }
    return false;
}

inline bool parse_live_partial_levels(
    std::string_view raw_array,
    std::size_t level_limit,
    bool bid_side,
    std::vector<provider::l2_snapshot::level>& out)
{
    if (level_limit == 0 || level_limit > 20) return false;

    out.clear();
    out.reserve(level_limit);

    std::size_t pos = 0;
    skip_json_ws(raw_array, pos);
    if (pos >= raw_array.size() || raw_array[pos++] != '[') return false;
    skip_json_ws(raw_array, pos);

    double previous_price = 0.0;
    bool have_previous_price = false;
    for (;;)
    {
        if (pos >= raw_array.size()) return false;
        if (raw_array[pos] == ']')
        {
            ++pos;
            break;
        }
        if (out.size() >= level_limit || raw_array[pos++] != '[')
            return false;

        std::string_view price_text;
        std::string_view quantity_text;
        if (!parse_decimal_json_string(raw_array, pos, price_text)) return false;
        skip_json_ws(raw_array, pos);
        if (pos >= raw_array.size() || raw_array[pos++] != ',') return false;
        if (!parse_decimal_json_string(raw_array, pos, quantity_text)) return false;
        skip_json_ws(raw_array, pos);
        // Exactly two strings belong to a level.  In particular, do not
        // silently skip a malformed or injected trailing element.
        if (pos >= raw_array.size() || raw_array[pos++] != ']') return false;

        double price = 0.0;
        double quantity = 0.0;
        if (!parse_plain_decimal(price_text, price)
            || !parse_plain_decimal(quantity_text, quantity)
            || !(price > 0.0) || !(quantity > 0.0))
            return false;

        // Keep conversion into provider::l2_snapshot::level defined even for
        // adversarially large numeric strings.  Nine quintillion atoms is
        // intentionally below int64_t's edge after double rounding and far
        // beyond a plausible Binance partial-book quantity.
        const double quantity_atoms = quantity * 1e8;
        if (!std::isfinite(quantity_atoms) || quantity_atoms < 1.0
            || quantity_atoms >= 9.0e18)
            return false;

        // Binance supplies descending bids and ascending asks.  A crossed or
        // duplicate level is an invalid replacement snapshot, not something
        // the live path should try to repair.
        if (have_previous_price
            && (bid_side ? !(price < previous_price)
                         : !(price > previous_price)))
            return false;
        previous_price = price;
        have_previous_price = true;
        out.push_back({price, static_cast<int64_t>(quantity_atoms)});

        skip_json_ws(raw_array, pos);
        if (pos >= raw_array.size()) return false;
        if (raw_array[pos] == ']')
        {
            ++pos;
            break;
        }
        if (raw_array[pos++] != ',') return false;
        skip_json_ws(raw_array, pos);
    }

    skip_json_ws(raw_array, pos);
    return pos == raw_array.size() && !out.empty();
}

inline bool is_ascii_binance_symbol(std::string_view symbol) noexcept
{
    if (symbol.empty() || symbol.size() > max_live_partial_book_symbol_bytes)
        return false;
    for (const unsigned char c : symbol)
    {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

inline bool ascii_symbol_equal_folded(std::string_view lhs,
                                      std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        const auto fold = [](unsigned char c) noexcept {
            return (c >= 'a' && c <= 'z')
                ? static_cast<unsigned char>(c - 'a' + 'A')
                : c;
        };
        if (fold(static_cast<unsigned char>(lhs[i]))
            != fold(static_cast<unsigned char>(rhs[i])))
            return false;
    }
    return true;
}

} // namespace depth_detail

// Parse a Binance depth payload (partial book or depthUpdate body) into an L2 snapshot.
// Zero heap allocs per level (string_view + from_chars); vectors reserve for depth20.
inline std::optional<provider::l2_snapshot> parse_depth_snapshot(std::string_view json)
{
    provider::l2_snapshot snap;

    auto sym_sv = extract_sv_string(json, "s");
    if (!sym_sv.empty())
        snap.symbol.assign(sym_sv.data(), sym_sv.size());

    snap.timestamp = depth_detail::parse_event_time(json);

    depth_detail::parse_side_levels(json, "bids", "b", snap.bids);
    depth_detail::parse_side_levels(json, "asks", "a", snap.asks);

    if (snap.bids.empty() && snap.asks.empty())
        return std::nullopt;

    return snap;
}

// Strict parser for the live partial-book contract.  `expected_symbol` comes
// from the authoritative combined-stream identity; if the payload happens to
// include `s`, it must agree with that identity.  This function intentionally
// does not accept raw depthUpdate shorthand (`b`/`a`) or a missing update id.
inline std::optional<provider::l2_snapshot> parse_strict_partial_book_snapshot(
    std::string_view payload,
    std::string_view expected_symbol,
    std::size_t level_limit)
{
    using member_result = provider_recovery::payload_parser::member_result;

    if (payload.empty()
        || payload.size() > depth_detail::max_live_partial_book_payload_bytes
        || !depth_detail::is_ascii_binance_symbol(expected_symbol)
        || (level_limit != 5 && level_limit != 10 && level_limit != 20)
        || !provider_recovery::is_authoritative_object(payload)
        || !provider_recovery::decision_members_are_unique(
            payload, {"lastUpdateId", "bids", "asks", "s", "e", "E"}))
        return std::nullopt;

    // A raw delta can otherwise be wrapped in a partial-looking envelope.
    // The documented partial contract has no event-type discriminator.
    std::string_view event_type;
    if (provider_recovery::payload_parser(payload).inspect_top_level_member(
            "e", event_type) != member_result::missing)
        return std::nullopt;

    std::string_view update_id_text;
    std::uint64_t update_id = 0;
    if (!provider_recovery::top_level_member(
            payload, "lastUpdateId", update_id_text)
        || !provider_recovery::parse_positive_u64(
            provider_recovery::trim_json_ws(update_id_text), update_id))
        return std::nullopt;
    (void)update_id; // validation proves the snapshot has an authoritative id

    std::string_view payload_symbol;
    const auto symbol_member = provider_recovery::payload_parser(payload)
        .inspect_top_level_member("s", payload_symbol);
    if (symbol_member == member_result::invalid_or_duplicate)
        return std::nullopt;
    if (symbol_member == member_result::unique)
    {
        if (!provider_recovery::top_level_plain_string(
                payload, "s", payload_symbol)
            || !depth_detail::is_ascii_binance_symbol(payload_symbol)
            || !depth_detail::ascii_symbol_equal_folded(
                payload_symbol, expected_symbol))
            return std::nullopt;
    }

    std::string_view bid_array;
    std::string_view ask_array;
    if (!provider_recovery::top_level_member(payload, "bids", bid_array)
        || !provider_recovery::top_level_member(payload, "asks", ask_array))
        return std::nullopt;

    provider::l2_snapshot snap;
    if (!depth_detail::parse_live_partial_levels(
            bid_array, level_limit, /*bid_side=*/true, snap.bids)
        || !depth_detail::parse_live_partial_levels(
            ask_array, level_limit, /*bid_side=*/false, snap.asks)
        || !(snap.bids.front().price < snap.asks.front().price))
        return std::nullopt;

    // Spot partial-book frames do not provide an event timestamp.  Preserve
    // the existing receive-time behavior rather than fabricating one from a
    // different field; any supplied `E` must still be a valid positive ms id.
    std::string_view event_time_text;
    const auto event_time_member = provider_recovery::payload_parser(payload)
        .inspect_top_level_member("E", event_time_text);
    if (event_time_member == member_result::invalid_or_duplicate)
        return std::nullopt;
    if (event_time_member == member_result::unique)
    {
        std::uint64_t event_time_ms = 0;
        if (!provider_recovery::parse_positive_u64(
                provider_recovery::trim_json_ws(event_time_text), event_time_ms)
            || event_time_ms
                > static_cast<std::uint64_t>(
                    std::numeric_limits<int64_t>::max()))
            return std::nullopt;
        snap.timestamp = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(static_cast<int64_t>(event_time_ms)));
    }
    else
    {
        snap.timestamp = std::chrono::system_clock::now();
    }
    snap.symbol.assign(expected_symbol.data(), expected_symbol.size());
    return snap;
}

// Expand depth payload directly into per-level l2_update events.
// Single pass over bids/asks — does NOT re-parse via snapshot + copy loop.
// Symbol is assigned once into a temporary and moved into the first update,
// then copied from that interned-size SSO string for the rest (still cheaper
// than re-extracting from JSON each time).
inline std::vector<provider::l2_update> parse_depth_updates(std::string_view json)
{
    std::vector<provider::l2_update> updates;

    auto ts = depth_detail::parse_event_time(json);
    auto sym_sv = extract_sv_string(json, "s");
    std::string symbol;
    if (!sym_sv.empty())
        symbol.assign(sym_sv.data(), sym_sv.size());

    // Parse levels into temporary vectors (price/qty only), then expand.
    // Cheaper than building a full l2_snapshot with the same data twice.
    std::vector<provider::l2_snapshot::level> bids;
    std::vector<provider::l2_snapshot::level> asks;
    depth_detail::parse_side_levels(json, "bids", "b", bids);
    depth_detail::parse_side_levels(json, "asks", "a", asks);

    updates.reserve(bids.size() + asks.size());

    auto push_side = [&](const std::vector<provider::l2_snapshot::level>& levels,
                         uint8_t side) {
        for (const auto& lvl : levels)
        {
            provider::l2_update upd;
            upd.timestamp = ts;
            upd.symbol = symbol; // SSO for typical symbols (e.g. BTCUSDT)
            upd.side = side;
            upd.price = lvl.price;
            upd.new_quantity = lvl.quantity;
            updates.push_back(std::move(upd));
        }
    };

    push_side(bids, /*bid=*/0);
    push_side(asks, /*ask=*/1);
    return updates;
}

} // namespace binance

class BinanceDepthSnapshotParser : public IDataParser<provider::l2_snapshot>
{
public:
    bool parse_header(const std::string&) override { return true; }

    std::optional<provider::l2_snapshot> parse_record(const std::string& line) override
    {
        return binance::parse_depth_snapshot(std::string_view{line});
    }

    std::optional<provider::l2_snapshot> parse_record(std::string_view line) override
    {
        return binance::parse_depth_snapshot(line);
    }
};
