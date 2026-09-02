#pragma once

#include "providers/parser.h"
#include "providers/provider_event.h"
#include "providers/binance/binance_parser.h"

#include <chrono>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace binance {

namespace depth_detail {

inline bool authoritative_depth_envelope(std::string_view json)
{
    return provider_recovery::is_authoritative_object(json)
        && provider_recovery::decision_members_are_unique(
            json, {"E", "s", "lastUpdateId", "U", "u", "pu",
                   "bids", "asks", "b", "a"});
}

inline bool find_level_array(std::string_view json, std::string_view key,
                             std::string_view& array, bool& present)
{
    array = {};
    present = false;
    const auto result = provider_recovery::payload_parser(json)
        .inspect_top_level_member(key, array);
    if (result
        == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
        return false;
    if (result == provider_recovery::payload_parser::member_result::missing)
        return true;
    array = provider_recovery::trim_json_ws(array);
    present = true;
    return array.size() >= 2 && array.front() == '[' && array.back() == ']';
}

// Parse one Binance depth level: ["price","qty"] starting at pos (on '[').
// Advances pos past the closing ']'. Returns false on malformed input.
inline bool parse_one_level(std::string_view json, std::size_t& pos,
                            double& price, std::int64_t& qty_atoms,
                            bool allow_zero_quantity)
{
    const std::size_t n = json.size();
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

    while (pos < n && json[pos] == ' ') ++pos;
    if (pos >= n || json[pos] != ',') return false;
    ++pos;
    while (pos < n && json[pos] == ' ') ++pos;

    // qty as "..."
    if (pos >= n || json[pos] != '"') return false;
    ++pos;
    auto qty_end = json.find('"', pos);
    if (qty_end == std::string_view::npos) return false;
    std::string_view qty_sv = json.substr(pos, qty_end - pos);
    pos = qty_end + 1;

    while (pos < n && json[pos] == ' ') ++pos;
    if (pos >= n || json[pos] != ']') return false;
    ++pos;

    if (!parse_double_sv(price_sv, price) || !(price > 0.0)) return false;
    const auto parsed_qty =
        tt::quantity_scale::parse_decimal_canonical_atoms(qty_sv);
    if (!parsed_qty || (!allow_zero_quantity && *parsed_qty == 0))
        return false;
    qty_atoms = *parsed_qty;
    return true;
}

// Append levels from `"key":[ ... ]` into out. Uses from_chars (no stod/substr allocs).
inline bool append_levels(std::string_view array,
                          std::vector<provider::l2_snapshot::level>& out,
                          bool allow_zero_quantity)
{
    std::size_t pos = 1;

    // Typical partial book is depth5/10/20 — reserve once.
    if (out.capacity() < out.size() + 20)
        out.reserve(out.size() + 20);

    const std::size_t n = array.size();
    bool first = true;
    while (pos < n)
    {
        while (pos < n && (array[pos] == ' ' || array[pos] == '\t'
                           || array[pos] == '\n' || array[pos] == '\r')) ++pos;
        if (pos >= n) return false;
        if (array[pos] == ']') return pos + 1 == n;
        if (!first)
        {
            if (array[pos] != ',') return false;
            ++pos;
            while (pos < n && (array[pos] == ' ' || array[pos] == '\t'
                               || array[pos] == '\n' || array[pos] == '\r')) ++pos;
            if (pos >= n || array[pos] == ']') return false;
        }

        double price = 0.0;
        std::int64_t qty_atoms = 0;
        if (!parse_one_level(
                array, pos, price, qty_atoms, allow_zero_quantity))
            return false;
        out.push_back({price, qty_atoms});
        first = false;
    }
    return false;
}

// Prefer long keys ("bids"/"asks"), fall back to short ("b"/"a").
inline bool parse_side_levels(std::string_view json,
                              std::string_view long_key, std::string_view short_key,
                              std::vector<provider::l2_snapshot::level>& out,
                              bool allow_zero_quantity)
{
    out.clear();
    std::string_view long_array;
    std::string_view short_array;
    bool has_long = false;
    bool has_short = false;
    if (!find_level_array(json, long_key, long_array, has_long)
        || !find_level_array(json, short_key, short_array, has_short)
        || (has_long && has_short))
        return false;
    if (!has_long && !has_short)
        return true;
    return append_levels(
        has_long ? long_array : short_array, out, allow_zero_quantity);
}

inline std::optional<std::chrono::system_clock::time_point>
parse_event_time(std::string_view json)
{
    std::string_view ts_sv;
    if (!provider_recovery::top_level_scalar_text(json, "E", ts_sv))
        return std::nullopt;
    int64_t ts_ms = 0;
    if (!parse_int64_sv(ts_sv, ts_ms))
        return std::nullopt;
    return tt::date_parse::from_epoch_milliseconds(ts_ms);
}

enum class optional_u64_result { missing, valid, invalid };

inline optional_u64_result parse_optional_nonzero_u64(
    std::string_view json, std::string_view key, std::uint64_t& out)
{
    std::string_view value;
    const auto state = provider_recovery::payload_parser(json)
        .inspect_top_level_member(key, value);
    if (state == provider_recovery::payload_parser::member_result::missing)
        return optional_u64_result::missing;
    if (state
            != provider_recovery::payload_parser::member_result::unique
        || !provider_recovery::top_level_scalar_text(json, key, value))
        return optional_u64_result::invalid;
    std::uint64_t parsed = 0;
    const auto [ptr, ec] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || ptr != value.data() + value.size() || parsed == 0)
        return optional_u64_result::invalid;
    out = parsed;
    return optional_u64_result::valid;
}

} // namespace depth_detail

// Parse a Binance depth payload (partial book or depthUpdate body) into an L2 snapshot.
// Zero heap allocs per level (string_view + from_chars); vectors reserve for depth20.
inline std::optional<provider::l2_snapshot> parse_depth_snapshot(std::string_view json)
{
    if (!depth_detail::authoritative_depth_envelope(json))
        return std::nullopt;
    provider::l2_snapshot snap;
    snap.quantity_scale = 100'000'000ULL;

    std::string_view sym_sv;
    if (provider_recovery::top_level_plain_string(json, "s", sym_sv)
        && !sym_sv.empty())
        snap.symbol.assign(sym_sv.data(), sym_sv.size());

    const auto timestamp = depth_detail::parse_event_time(json);
    if (!timestamp)
        return std::nullopt;
    snap.timestamp = *timestamp;
    const auto snapshot_id = depth_detail::parse_optional_nonzero_u64(
        json, "lastUpdateId", snap.last_update_id);
    if (snapshot_id == depth_detail::optional_u64_result::invalid)
        return std::nullopt;

    if (!depth_detail::parse_side_levels(
            json, "bids", "b", snap.bids, false)
        || !depth_detail::parse_side_levels(
            json, "asks", "a", snap.asks, false))
        return std::nullopt;

    if (snap.bids.empty() && snap.asks.empty())
        return std::nullopt;

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
    if (!depth_detail::authoritative_depth_envelope(json))
        return updates;

    const auto timestamp = depth_detail::parse_event_time(json);
    if (!timestamp)
        return {};
    const auto ts = *timestamp;
    std::string_view sym_sv;
    if (!provider_recovery::top_level_plain_string(json, "s", sym_sv)
        || sym_sv.empty())
        return {};
    std::string symbol;
    if (!sym_sv.empty())
        symbol.assign(sym_sv.data(), sym_sv.size());

    // Parse levels into temporary vectors (price/qty only), then expand.
    // Cheaper than building a full l2_snapshot with the same data twice.
    std::vector<provider::l2_snapshot::level> bids;
    std::vector<provider::l2_snapshot::level> asks;
    if (!depth_detail::parse_side_levels(
            json, "bids", "b", bids, true)
        || !depth_detail::parse_side_levels(
            json, "asks", "a", asks, true))
        return {};

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
            upd.quantity_scale = 100'000'000ULL;
            updates.push_back(std::move(upd));
        }
    };

    push_side(bids, /*bid=*/0);
    push_side(asks, /*ask=*/1);
    return updates;
}

inline std::optional<provider::l2_delta_batch>
parse_depth_delta_batch(std::string_view json)
{
    if (!depth_detail::authoritative_depth_envelope(json))
        return std::nullopt;
    provider::l2_delta_batch batch;
    const auto timestamp = depth_detail::parse_event_time(json);
    if (!timestamp)
        return std::nullopt;
    batch.timestamp = *timestamp;
    batch.quantity_scale = tt::quantity_scale::canonical_atoms;
    std::string_view symbol;
    if (!provider_recovery::top_level_plain_string(json, "s", symbol))
        return std::nullopt;
    if (symbol.empty()
        || depth_detail::parse_optional_nonzero_u64(
               json, "U", batch.first_update_id)
            != depth_detail::optional_u64_result::valid
        || depth_detail::parse_optional_nonzero_u64(
               json, "u", batch.final_update_id)
            != depth_detail::optional_u64_result::valid
        || batch.first_update_id > batch.final_update_id)
        return std::nullopt;
    batch.symbol.assign(symbol.data(), symbol.size());

    std::uint64_t previous = 0;
    const auto previous_state = depth_detail::parse_optional_nonzero_u64(
        json, "pu", previous);
    if (previous_state == depth_detail::optional_u64_result::invalid)
        return std::nullopt;
    if (previous_state == depth_detail::optional_u64_result::valid)
    {
        batch.previous_final_update_id = previous;
        batch.has_previous_final_update_id = true;
    }

    batch.updates = parse_depth_updates(json);
    if (batch.updates.empty()) return std::nullopt;
    for (auto& update : batch.updates)
    {
        update.timestamp = batch.timestamp;
        update.symbol = batch.symbol;
        update.quantity_scale = batch.quantity_scale;
    }
    return batch;
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
