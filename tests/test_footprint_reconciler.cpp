// footprint.md §2.2 reconciliation state machine: BACKFILLING -> LIVE,
// RECOVERING -> (repaired) LIVE or -> PARTIAL, dedup across cache/history/
// live, the two-second reorder window, and "never mutate already-published
// historical bars silently".

#include <gtest/gtest.h>

#include "analytics/footprint/footprint_reconciler.h"

using namespace truetest::footprint;

namespace {
constexpr std::int64_t kSecond = 1'000'000'000LL;

PublicTrade make_native(std::int64_t event_ns, std::uint64_t native_id,
                         std::int64_t price_ticks = 100)
{
    PublicTrade t;
    t.event_ns = event_ns;
    t.recv_ns = event_ns;
    t.native_trade_id = native_id;
    t.flags = provenance_native_id;
    t.price_ticks = price_ticks;
    t.base_qty_atoms = 1;
    return t;
}

PublicTrade make_session(std::int64_t event_ns, std::uint64_t session_id, std::uint64_t obs_seq)
{
    PublicTrade t;
    t.event_ns = event_ns;
    t.recv_ns = event_ns;
    t.session_id = session_id;
    t.obs_seq = obs_seq;
    t.flags = provenance_session_only;
    t.price_ticks = 100;
    t.base_qty_atoms = 1;
    return t;
}
} // namespace

TEST(FootprintReconciler, StartsUnavailableWithNoTrades)
{
    FootprintReconciler r;
    EXPECT_EQ(r.status(), data_status::unavailable);
    EXPECT_TRUE(r.verified_trades().empty());
}

TEST(FootprintReconciler, LoadCachePublishesImmediatelyAsBackfilling)
{
    FootprintReconciler r;
    r.load_cache({make_native(0, 1), make_native(kSecond, 2)});
    EXPECT_EQ(r.status(), data_status::backfilling);
    ASSERT_EQ(r.verified_trades().size(), 2u);
    EXPECT_EQ(r.verified_trades()[0].native_trade_id, 1u);
}

TEST(FootprintReconciler, LoadCacheSortsAndDedupesDefensively)
{
    FootprintReconciler r;
    // Out of order, with a duplicate native id.
    r.load_cache({make_native(2 * kSecond, 2), make_native(0, 1), make_native(2 * kSecond, 2)});
    ASSERT_EQ(r.verified_trades().size(), 2u);
    EXPECT_EQ(r.verified_trades()[0].event_ns, 0);
    EXPECT_EQ(r.verified_trades()[1].event_ns, 2 * kSecond);
}

TEST(FootprintReconciler, LiveTradesBufferDuringBackfillingNotYetPublished)
{
    FootprintReconciler r;
    r.load_cache({make_native(0, 1)});
    r.on_live_trade(make_native(100 * kSecond, 2));
    // Pushes the watermark far enough past trade #2's event_ns that the
    // reorder window actually drains it into the pending live buffer.
    r.on_live_trade(make_native(103 * kSecond, 3));

    // Not yet merged - complete_backfill hasn't run.
    EXPECT_EQ(r.status(), data_status::backfilling);
    ASSERT_EQ(r.verified_trades().size(), 1u); // still just the cache
    EXPECT_GT(r.pending_live_count(), 0u);
}

TEST(FootprintReconciler, CompleteBackfillMergesCacheHistoryAndBufferedLiveIntoLive)
{
    FootprintReconciler r;
    r.load_cache({make_native(0, 1)});
    r.on_live_trade(make_native(100 * kSecond, 3));
    // Drain the reorder window so the buffered live trade is actually ready
    // (default window is 2s; feed a far-later trade to push the watermark).
    r.on_live_trade(make_native(200 * kSecond, 4));

    r.complete_backfill({make_native(kSecond, 2)}, /*contiguous_with_cache=*/true);

    EXPECT_EQ(r.status(), data_status::live);
    // cache(1) + history(2) + buffered-live(3,4) = 4 unique trades, sorted.
    const auto& v = r.verified_trades();
    ASSERT_EQ(v.size(), 4u);
    for (std::size_t i = 1; i < v.size(); ++i)
        EXPECT_LE(v[i - 1].event_ns, v[i].event_ns);
    EXPECT_EQ(r.pending_live_count(), 0u);
}

TEST(FootprintReconciler, CompleteBackfillDedupesOverlapBetweenHistoryAndCache)
{
    FootprintReconciler r;
    r.load_cache({make_native(0, 1), make_native(kSecond, 2)});
    // History re-fetches the same range, including a duplicate of native id 2.
    r.complete_backfill({make_native(kSecond, 2), make_native(2 * kSecond, 3)}, true);

    ASSERT_EQ(r.verified_trades().size(), 3u); // 1,2,3 - not 4
}

TEST(FootprintReconciler, MissingOverlapEntersRecoveringAndFreezesLastVerified)
{
    FootprintReconciler r;
    r.load_cache({make_native(0, 1)});
    r.complete_backfill({make_native(100 * kSecond, 2)}, /*contiguous_with_cache=*/false);

    EXPECT_EQ(r.status(), data_status::recovering);
    // Frozen - still exactly the cache, the failed history fetch never applied.
    ASSERT_EQ(r.verified_trades().size(), 1u);
    EXPECT_EQ(r.verified_trades()[0].native_trade_id, 1u);
}

TEST(FootprintReconciler, OnFaultIsIdempotentWhileAlreadyRecovering)
{
    FootprintReconciler r;
    r.load_cache({make_native(0, 1)});
    r.on_fault(FootprintReconciler::fault_kind::disconnect);
    ASSERT_EQ(r.status(), data_status::recovering);
    const auto verified_before = r.verified_trades();

    r.on_fault(FootprintReconciler::fault_kind::overflow); // must not churn anything
    EXPECT_EQ(r.status(), data_status::recovering);
    EXPECT_EQ(r.verified_trades().size(), verified_before.size());
}

TEST(FootprintReconciler, ReorderWindowViolationIsAFaultAndDropsTheLateTrade)
{
    FootprintReconciler r;
    r.load_cache({});
    r.on_live_trade(make_native(100 * kSecond, 1)); // watermark now 100s
    EXPECT_EQ(r.status(), data_status::backfilling); // no fault yet

    r.on_live_trade(make_native(10 * kSecond, 2)); // 90s late - beyond the 2s window -> rejected
    EXPECT_EQ(r.status(), data_status::recovering);

    // The too-late trade was never buffered anywhere - confirm by
    // completing backfill and checking it never appears in the result.
    r.complete_backfill({}, true);
    for (const auto& t : r.verified_trades())
        EXPECT_NE(t.native_trade_id, 2u);
}

TEST(FootprintReconciler, LiveBufferOverflowTriggersOverflowFaultAndCapsGrowth)
{
    FootprintReconciler::Config cfg;
    cfg.live_buffer_capacity = 3;
    FootprintReconciler r(cfg);
    r.load_cache({});

    for (std::uint64_t i = 0; i < 10; ++i)
    {
        // Space arrivals well beyond the reorder window so each one drains
        // immediately into the pending buffer.
        r.on_live_trade(make_native(static_cast<std::int64_t>(i) * 100 * kSecond, i));
    }

    EXPECT_EQ(r.status(), data_status::recovering);
    EXPECT_LE(r.pending_live_count(), 3u); // never grows past the configured cap
}

TEST(FootprintReconciler, RepairSucceededMergesFrozenPlusRepairedPlusBufferedLiveBackToLive)
{
    FootprintReconciler r;
    r.load_cache({make_native(0, 1)});
    r.complete_backfill({make_native(kSecond, 2)}, true); // now LIVE, verified={1,2}
    ASSERT_EQ(r.status(), data_status::live);

    r.on_fault(FootprintReconciler::fault_kind::disconnect); // -> RECOVERING, frozen at {1,2}
    r.on_live_trade(make_native(3 * kSecond, 3));
    r.on_live_trade(make_native(4 * kSecond, 4)); // drains trade 3 into pending buffer

    r.repair_succeeded({make_native(2 * kSecond + kSecond / 2, 99)}); // the gap-filling data

    EXPECT_EQ(r.status(), data_status::live);
    // {1,2} frozen + repaired{99} + buffered-live{3} = 4 (trade 4 still pending/not yet drained).
    ASSERT_GE(r.verified_trades().size(), 4u);
    EXPECT_FALSE(r.gap().has_value());
}

TEST(FootprintReconciler, RepairFailedReportsPartialWithExactGapAndLeavesVerifiedUntouched)
{
    FootprintReconciler r;
    r.load_cache({make_native(0, 1)});
    r.on_fault(FootprintReconciler::fault_kind::corrupt_segment);
    const auto before = r.verified_trades();

    r.repair_failed(5 * kSecond, 10 * kSecond);

    EXPECT_EQ(r.status(), data_status::partial);
    ASSERT_TRUE(r.gap().has_value());
    EXPECT_EQ(r.gap()->first, 5 * kSecond);
    EXPECT_EQ(r.gap()->second, 10 * kSecond);
    EXPECT_EQ(r.verified_trades().size(), before.size());
    EXPECT_EQ(r.verified_trades()[0].native_trade_id, before[0].native_trade_id);
}

TEST(FootprintReconciler, SteadyStateLiveTradesAppendDirectlyOnceLive)
{
    FootprintReconciler r;
    r.load_cache({});
    r.complete_backfill({}, true);
    ASSERT_EQ(r.status(), data_status::live);

    r.on_live_trade(make_native(kSecond, 1));
    r.on_live_trade(make_native(2 * kSecond, 2));
    r.on_live_trade(make_native(3 * kSecond, 3)); // watermark=3s -> drains trade #1 (1s <= 3s-2s=1s)
    r.on_live_trade(make_native(4 * kSecond, 4)); // watermark=4s -> drains trade #2 (2s <= 4s-2s=2s)

    EXPECT_GE(r.verified_trades().size(), 2u);
    EXPECT_EQ(r.pending_live_count(), 0u); // steady-state never uses the pending buffer
}

TEST(FootprintReconciler, SteadyStateLiveAppendDedupesDuplicateArrivals)
{
    FootprintReconciler r;
    r.load_cache({});
    r.complete_backfill({}, true);

    r.on_live_trade(make_native(kSecond, 1));
    r.on_live_trade(make_native(kSecond + kSecond, 1)); // same native id, later event_ns - a dup
    r.on_live_trade(make_native(5 * kSecond, 2));        // pushes the watermark to drain the above

    // Only one entry for native id 1, despite two arrivals.
    std::size_t count_id1 = 0;
    for (const auto& t : r.verified_trades())
        if (t.native_trade_id == 1) ++count_id1;
    EXPECT_EQ(count_id1, 1u);
}

TEST(FootprintReconciler, NeverMutatesAlreadyPublishedEntriesInPlace)
{
    FootprintReconciler r;
    r.load_cache({make_native(0, 1)});
    r.complete_backfill({make_native(kSecond, 2)}, true);
    const auto snapshot = r.verified_trades(); // copy

    r.on_fault(FootprintReconciler::fault_kind::disconnect);
    r.on_live_trade(make_native(2 * kSecond, 3));
    r.on_live_trade(make_native(3 * kSecond, 4));
    r.repair_succeeded({});

    // The original two entries must still be present, unchanged, at the front.
    const auto& now = r.verified_trades();
    ASSERT_GE(now.size(), snapshot.size());
    for (std::size_t i = 0; i < snapshot.size(); ++i)
    {
        EXPECT_EQ(now[i].native_trade_id, snapshot[i].native_trade_id);
        EXPECT_EQ(now[i].event_ns, snapshot[i].event_ns);
    }
}

TEST(FootprintReconciler, SessionOnlyVenueDedupesOnSessionAndObsSeqNotAcrossSessions)
{
    FootprintReconciler r;
    r.load_cache({make_session(0, /*session=*/1, /*obs_seq=*/0)});
    // Reconnect: new session, same obs_seq=0 - must NOT be treated as a
    // duplicate of the cached trade (footprint.md §2.1).
    r.complete_backfill({make_session(kSecond, /*session=*/2, /*obs_seq=*/0)}, true);

    EXPECT_EQ(r.verified_trades().size(), 2u);
}
