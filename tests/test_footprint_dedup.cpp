#include <gtest/gtest.h>

#include "analytics/footprint/footprint_dedup.h"

#include <unordered_set>

using namespace truetest::footprint;

namespace {
PublicTrade make_native(std::uint16_t venue, std::uint16_t symbol, std::uint64_t native_id)
{
    PublicTrade t;
    t.venue_id = venue;
    t.symbol_id = symbol;
    t.native_trade_id = native_id;
    t.flags = provenance_native_id;
    return t;
}

PublicTrade make_session(std::uint16_t venue, std::uint16_t symbol,
                          std::uint64_t session_id, std::uint64_t obs_seq)
{
    PublicTrade t;
    t.venue_id = venue;
    t.symbol_id = symbol;
    t.session_id = session_id;
    t.obs_seq = obs_seq;
    t.flags = provenance_session_only;
    return t;
}
} // namespace

TEST(FootprintDedup, NativeIdTradesWithSameKeyAreEqual)
{
    auto a = dedup_key_of(make_native(1, 1, 42));
    auto b = dedup_key_of(make_native(1, 1, 42));
    EXPECT_EQ(a, b);
}

TEST(FootprintDedup, NativeIdTradesWithDifferentIdDiffer)
{
    auto a = dedup_key_of(make_native(1, 1, 42));
    auto b = dedup_key_of(make_native(1, 1, 43));
    EXPECT_FALSE(a == b);
}

TEST(FootprintDedup, SessionOnlyTradesWithSameSessionAndSeqAreEqual)
{
    auto a = dedup_key_of(make_session(3, 1, 7, 100));
    auto b = dedup_key_of(make_session(3, 1, 7, 100));
    EXPECT_EQ(a, b);
}

TEST(FootprintDedup, SessionOnlyTradesFromDifferentSessionsNeverMatchEvenWithSameSeq)
{
    // A reconnect bumps session_id - the same obs_seq in a new session must
    // never be treated as a duplicate of the old one (footprint.md §2.1:
    // reconnects are explicit continuity boundaries, never silently bridged).
    auto a = dedup_key_of(make_session(3, 1, 7, 100));
    auto b = dedup_key_of(make_session(3, 1, 8, 100));
    EXPECT_FALSE(a == b);
}

TEST(FootprintDedup, DifferentSymbolsNeverMatch)
{
    auto a = dedup_key_of(make_native(1, 1, 42));
    auto b = dedup_key_of(make_native(1, 2, 42));
    EXPECT_FALSE(a == b);
}

TEST(FootprintDedup, DifferentVenuesNeverMatch)
{
    auto a = dedup_key_of(make_native(1, 1, 42));
    auto b = dedup_key_of(make_native(2, 1, 42));
    EXPECT_FALSE(a == b);
}

TEST(FootprintDedup, NativeAndSessionOnlyNeverCollideEvenWithOverlappingRawFields)
{
    // native_trade_id=7 vs (session_id=7, obs_seq=0) - same raw numbers,
    // different `native` flag must keep them distinct.
    auto a = dedup_key_of(make_native(1, 1, 7));
    auto b = dedup_key_of(make_session(1, 1, 7, 0));
    EXPECT_FALSE(a == b);
}

TEST(FootprintDedup, WorksAsUnorderedSetKey)
{
    std::unordered_set<TradeDedupKey> seen;
    EXPECT_TRUE(seen.insert(dedup_key_of(make_native(1, 1, 1))).second);
    EXPECT_FALSE(seen.insert(dedup_key_of(make_native(1, 1, 1))).second); // duplicate
    EXPECT_TRUE(seen.insert(dedup_key_of(make_native(1, 1, 2))).second);
    EXPECT_EQ(seen.size(), 2u);
}
