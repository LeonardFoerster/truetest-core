#pragma once

#include "providers/parser.h"
#include "providers/provider_event.h"
#include "providers/binance/binance_parser.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace binance {

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
inline bool append_levels(std::string_view json, std::string_view key,
                          std::vector<provider::l2_snapshot::level>& out)
{
    auto pos = find_level_array(json, key);
    if (pos == std::string_view::npos) return true;

    // Typical partial book is depth5/10/20 — reserve once.
    if (out.capacity() < out.size() + 20)
        out.reserve(out.size() + 20);

    const std::size_t n = json.size();
    while (pos < n)
    {
        double price = 0.0, qty = 0.0;
        if (!parse_one_level(json, pos, price, qty))
        {
            // End of array is valid; anything else rejects the whole frame.
            while (pos < n && (json[pos] == ' ' || json[pos] == ',')) ++pos;
            return pos < n && json[pos] == ']';
        }
        std::int64_t qty_atoms = 0;
        if (!(price > 0.0)
            || !tt::quantity_scale::from_base_nonnegative(
                qty, tt::quantity_scale::canonical_atoms, qty_atoms))
            return false;
        out.push_back({price, qty_atoms});

        while (pos < n && (json[pos] == ' ' || json[pos] == ',')) ++pos;
        if (pos >= n || json[pos] == ']')
            break;
    }
    return true;
}

// Prefer long keys ("bids"/"asks"), fall back to short ("b"/"a").
inline bool parse_side_levels(std::string_view json,
                              std::string_view long_key, std::string_view short_key,
                              std::vector<provider::l2_snapshot::level>& out)
{
    out.clear();
    if (find_level_array(json, long_key) != std::string_view::npos)
        return append_levels(json, long_key, out);
    return append_levels(json, short_key, out);
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

} // namespace depth_detail

// Parse a Binance depth payload (partial book or depthUpdate body) into an L2 snapshot.
// Zero heap allocs per level (string_view + from_chars); vectors reserve for depth20.
inline std::optional<provider::l2_snapshot> parse_depth_snapshot(std::string_view json)
{
    provider::l2_snapshot snap;
    snap.quantity_scale = 100'000'000ULL;

    auto sym_sv = extract_sv_string(json, "s");
    if (!sym_sv.empty())
        snap.symbol.assign(sym_sv.data(), sym_sv.size());

    snap.timestamp = depth_detail::parse_event_time(json);

    if (!depth_detail::parse_side_levels(json, "bids", "b", snap.bids)
        || !depth_detail::parse_side_levels(json, "asks", "a", snap.asks))
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

    auto ts = depth_detail::parse_event_time(json);
    auto sym_sv = extract_sv_string(json, "s");
    std::string symbol;
    if (!sym_sv.empty())
        symbol.assign(sym_sv.data(), sym_sv.size());

    // Parse levels into temporary vectors (price/qty only), then expand.
    // Cheaper than building a full l2_snapshot with the same data twice.
    std::vector<provider::l2_snapshot::level> bids;
    std::vector<provider::l2_snapshot::level> asks;
    if (!depth_detail::parse_side_levels(json, "bids", "b", bids)
        || !depth_detail::parse_side_levels(json, "asks", "a", asks))
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
