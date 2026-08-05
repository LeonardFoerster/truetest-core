#pragma once

#include "types/public_trade.h"

#include <cstdint>
#include <functional>

// footprint.md §2.2: "deduplicate native-ID venues" when merging
// cache/history/live trade sources that may overlap. Native-ID venues
// (Binance, Bitget) dedupe on (venue_id, symbol_id, native_trade_id);
// session-only venues (Bitunix, §2.1) have no stitchable identity across a
// reconnect, so they dedupe on (venue_id, symbol_id, session_id, obs_seq)
// instead - two trades from different sessions are never considered
// duplicates even if every other field happens to match.
namespace truetest::footprint {

struct TradeDedupKey
{
    std::uint16_t venue_id = 0;
    std::uint16_t symbol_id = 0;
    bool native = false;
    std::uint64_t a = 0; // native_trade_id when native, else session_id
    std::uint64_t b = 0; // 0 when native, else obs_seq

    bool operator==(const TradeDedupKey& other) const noexcept
    {
        return venue_id == other.venue_id && symbol_id == other.symbol_id
            && native == other.native && a == other.a && b == other.b;
    }
};

inline TradeDedupKey dedup_key_of(const PublicTrade& t) noexcept
{
    TradeDedupKey k;
    k.venue_id = t.venue_id;
    k.symbol_id = t.symbol_id;
    if (t.flags & provenance_native_id)
    {
        k.native = true;
        k.a = t.native_trade_id;
        k.b = 0;
    }
    else
    {
        k.native = false;
        k.a = t.session_id;
        k.b = t.obs_seq;
    }
    return k;
}

} // namespace truetest::footprint

namespace std {
template <>
struct hash<truetest::footprint::TradeDedupKey>
{
    std::size_t operator()(const truetest::footprint::TradeDedupKey& k) const noexcept
    {
        std::size_t h = std::hash<std::uint64_t>{}(k.a);
        h ^= std::hash<std::uint64_t>{}(k.b) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        const auto venue_symbol =
            (static_cast<std::uint32_t>(k.venue_id) << 16) | k.symbol_id;
        h ^= std::hash<std::uint32_t>{}(venue_symbol) + 0x9e3779b9U + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.native) + 0x9e3779b9U + (h << 6) + (h >> 2);
        return h;
    }
};
} // namespace std
