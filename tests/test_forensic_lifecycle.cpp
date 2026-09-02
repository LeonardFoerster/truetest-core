// Forensic trade-lifecycle audit regressions — docs/todos/11-F-forensic-lifecycle-audit.md
//
// Every test here is RED against the pre-fix binary. They pin the exit
// lifecycle defects the 2026-08-22 runtime-trace audit proved on the
// ema-rsi-atr-pullback strategy:
//
//   F-01(a) entry-slippage shift is unbounded  -> phantom stop-outs
//   F-01(b) gap-at-open applied to intents armed inside the same bar
//   F-03    opener fill precedes intent registration -> bracket never arms
//   F-06    pending intents leak and are unbounded
//   F-09a   multi-level opener fills leave SL/TP on the first partial

#include <gtest/gtest.h>

#include "exits/exit_manager.h"
#include "core/event.h"

#include <chrono>
#include <string>

using namespace truetest::exits;
using tp = std::chrono::system_clock::time_point;

namespace {

const tp kT0 = tp{std::chrono::seconds{1'700'000'000}};

// Entry-relative long: SL/TP are declared against `ref`, so ExitManager is
// expected to preserve the designed |ref - sl| distance under entry slippage.
exit_intent long_intent(std::uint64_t opener, double ref, double sl, double tp_px,
                        double qty = 1.0, const std::string& sym = "BTCUSDT")
{
    exit_intent ei;
    ei.strategy_name   = "forensic";
    ei.symbol          = sym;
    ei.close_side      = order_side::sell;
    ei.qty             = qty;
    ei.reference_entry = ref;
    ei.stop_loss       = sl;
    ei.take_profit     = tp_px;
    ei.opener_order_id = opener;
    return ei;
}

exit_intent short_intent(std::uint64_t opener, double ref, double sl, double tp_px,
                         double qty = 1.0, const std::string& sym = "BTCUSDT")
{
    exit_intent ei = long_intent(opener, ref, sl, tp_px, qty, sym);
    ei.close_side = order_side::buy;
    return ei;
}

fill_event opener_fill(std::uint64_t id, order_side side, double qty, double px,
                       tp ts = kT0, const std::string& sym = "BTCUSDT")
{
    return fill_event(ts, sym, id, side, qty, px, /*commission=*/0.0,
                      /*remaining=*/0.0, /*fill_id=*/id);
}

double armed_stop(const ExitManager& m, std::uint64_t opener)
{
    for (const auto& v : m.snapshot_armed())
        if (v.opener_order_id == opener && v.stop_loss)
            return *v.stop_loss;
    return 0.0;
}

double armed_entry(const ExitManager& m, std::uint64_t opener)
{
    for (const auto& v : m.snapshot_armed())
        if (v.opener_order_id == opener)
            return v.entry_price;
    return 0.0;
}

}  // namespace

// ── F-01(a): unbounded entry-slippage shift ────────────────────────────────

TEST(ForensicLifecycle, F01a_SlippageInsideDesignedStopStillShifts)
{
    ExitManager m;
    // Designed risk 1.00; the fill slips 0.40 — well inside it.
    m.register_pending(long_intent(1, /*ref=*/100.0, /*sl=*/99.0, /*tp=*/103.0));
    m.on_fill(opener_fill(1, order_side::buy, 1.0, 100.40));

    ASSERT_EQ(m.armed_count(), 1u);
    EXPECT_NEAR(armed_stop(m, 1), 99.40, 1e-9)
        << "an in-budget slippage must still preserve the designed distance";
    EXPECT_EQ(m.counters().slippage_disarms, 0u);
    EXPECT_FALSE(m.has_flatten_requests());
}

TEST(ForensicLifecycle, F01a_SlippageBeyondDesignedStopRefusesBracketAndFlattens)
{
    ExitManager m;
    // Designed risk 1.00; the fill slips 1.50. The old code shifted the stop
    // to 100.50 — above the market that produced the fill — so the very next
    // bar low stopped the trade out at a price it never traded at.
    m.register_pending(long_intent(1, /*ref=*/100.0, /*sl=*/99.0, /*tp=*/103.0, 2.0));
    m.on_fill(opener_fill(1, order_side::buy, 2.0, 101.50));

    EXPECT_EQ(m.armed_count(), 0u) << "the trade's risk premise is void; do not arm";
    EXPECT_EQ(m.pending_count(), 0u);
    EXPECT_EQ(m.counters().slippage_disarms, 1u);

    ASSERT_TRUE(m.has_flatten_requests());
    auto reqs = m.take_flatten_requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].opener_order_id, 1u);
    EXPECT_EQ(reqs[0].close_side, order_side::sell);
    EXPECT_NEAR(reqs[0].qty, 2.0, 1e-12);
    EXPECT_NEAR(reqs[0].designed_stop_distance, 1.0, 1e-12);
    EXPECT_NEAR(reqs[0].entry_slippage, 1.50, 1e-12);
    EXPECT_FALSE(m.has_flatten_requests()) << "requests are drained exactly once";

    // No armed bracket means no phantom stop-out can follow.
    EXPECT_TRUE(m.on_bar("BTCUSDT", 101.5, 90.0, 102.0, 95.0, kT0).empty());
}

TEST(ForensicLifecycle, F01a_ShortMirrorRefusesBracketAndFlattens)
{
    ExitManager m;
    // Short: designed risk 1.00 above the reference; the fill slips 1.50 down.
    m.register_pending(short_intent(7, /*ref=*/100.0, /*sl=*/101.0, /*tp=*/97.0, 3.0));
    m.on_fill(opener_fill(7, order_side::sell, 3.0, 98.50));

    EXPECT_EQ(m.armed_count(), 0u);
    EXPECT_EQ(m.counters().slippage_disarms, 1u);
    auto reqs = m.take_flatten_requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].close_side, order_side::buy) << "flattening a short buys back";
    EXPECT_NEAR(reqs[0].qty, 3.0, 1e-12);
}

TEST(ForensicLifecycle, F01a_SiblingIntentIsNotArmedOnAVoidPremise)
{
    ExitManager m;
    // TP1/SL scale-out: both siblings ride the same entry premise.
    auto tp1 = long_intent(5, 100.0, 99.0, 101.0);
    tp1.qty_fraction = 0.5;
    auto runner = long_intent(5, 100.0, 99.0, 105.0);
    runner.qty_fraction = 0.5;
    m.register_pending(tp1);
    m.register_pending(runner);
    m.on_fill(opener_fill(5, order_side::buy, 4.0, 101.75));

    EXPECT_EQ(m.armed_count(), 0u)
        << "no sibling may survive a disarm of the premise they share";
    EXPECT_EQ(m.counters().flatten_requests, 1u);
}

TEST(ForensicLifecycle, F01a_AbsoluteStructureStopIsNeverDisarmed)
{
    ExitManager m;
    // No reference_entry: an absolute structure stop carries no entry-relative
    // premise, so slippage cannot void it and it must arm untouched.
    exit_intent ei;
    ei.strategy_name   = "forensic";
    ei.symbol          = "BTCUSDT";
    ei.close_side      = order_side::sell;
    ei.qty             = 1.0;
    ei.stop_loss       = 90.0;
    ei.opener_order_id = 3;
    m.register_pending(ei);
    m.on_fill(opener_fill(3, order_side::buy, 1.0, 120.0));

    ASSERT_EQ(m.armed_count(), 1u);
    EXPECT_NEAR(armed_stop(m, 3), 90.0, 1e-12);
    EXPECT_EQ(m.counters().slippage_disarms, 0u);
}

// ── F-01(b): gap-at-open applied to same-observation arms ──────────────────

TEST(ForensicLifecycle, F01b_SameBarArmDoesNotFillAtAPreArmOpen)
{
    ExitManager m;
    m.begin_evaluation_window();          // bar N opens
    m.register_pending(long_intent(1, 100.0, 99.0, 103.0));
    // The entry fills at 100.50 partway into a bar that opened at 99.00, so
    // the shifted stop at 99.50 is already above this bar's open. Pre-fix the
    // SL_GAP_AT_OPEN branch read that as a gap and anchored the exit at 99.00
    // — a price printed before this bracket existed.
    m.on_fill(opener_fill(1, order_side::buy, 1.0, 100.50));
    ASSERT_EQ(m.armed_count(), 1u);
    EXPECT_NEAR(armed_stop(m, 1), 99.50, 1e-9);

    auto fires = m.on_bar("BTCUSDT", 99.0, 97.0, 101.0, 98.0, kT0);
    ASSERT_EQ(fires.size(), 1u) << "the lot must still be protected on its own bar";
    EXPECT_NEAR(fires[0].get_price(), 99.50, 1e-9)
        << "fills at the level, never at an open the bracket predates";

}

TEST(ForensicLifecycle, F01b_SameBarArmIsStillProtectedByItsStop)
{
    ExitManager m;
    m.begin_evaluation_window();
    m.register_pending(long_intent(1, 100.0, 99.0, 103.0));
    m.on_fill(opener_fill(1, order_side::buy, 1.0, 100.0));   // fills at bar N open

    // The bar wicks through the stop *after* the entry filled at its open.
    // Deferring the whole bar would leave the lot unprotected for exactly
    // the bar it was opened in.
    auto fires = m.on_bar("BTCUSDT", 100.0, 98.0, 100.5, 98.5, kT0);
    ASSERT_EQ(fires.size(), 1u);
    EXPECT_NEAR(fires[0].get_price(), 99.0, 1e-9);
}

TEST(ForensicLifecycle, F01b_NextWindowStillFiresAtTheStopLevel)
{
    ExitManager m;
    m.begin_evaluation_window();
    m.register_pending(long_intent(1, 100.0, 99.0, 103.0));
    m.on_fill(opener_fill(1, order_side::buy, 1.0, 100.0));
    (void)m.on_bar("BTCUSDT", 100.0, 99.5, 100.5, 100.2, kT0);

    m.begin_evaluation_window();          // bar N+1
    auto fires = m.on_bar("BTCUSDT", 100.2, 98.0, 100.4, 98.4, kT0);
    ASSERT_EQ(fires.size(), 1u);
    EXPECT_NEAR(fires[0].get_price(), 99.0, 1e-9)
        << "an ordinary intra-bar trigger fills at the stop level";
}

TEST(ForensicLifecycle, F01b_SameBarArmNeverFillsOutsideTheBarRange)
{
    ExitManager m;
    m.begin_evaluation_window();
    m.register_pending(long_intent(1, 100.0, 99.0, 103.0));
    m.on_fill(opener_fill(1, order_side::buy, 1.0, 100.0));

    // Degenerate bar: the whole range sits below the stop. Anchoring at the
    // level would fill at a price this bar never traded at.
    auto fires = m.on_bar("BTCUSDT", 98.5, 97.0, 98.6, 97.2, kT0);
    ASSERT_EQ(fires.size(), 1u);
    EXPECT_GE(fires[0].get_price(), 97.0);
    EXPECT_LE(fires[0].get_price(), 98.6);
}


TEST(ForensicLifecycle, F01b_GenuineGapStillFillsAtTheOpen)
{
    ExitManager m;
    m.begin_evaluation_window();
    m.register_pending(long_intent(1, 100.0, 99.0, 103.0));
    m.on_fill(opener_fill(1, order_side::buy, 1.0, 100.0));
    (void)m.on_bar("BTCUSDT", 100.0, 99.5, 100.5, 100.2, kT0);

    // Bar N+1 opens *through* the stop that has existed since bar N. That is
    // a real gap and must still fill at the open, not at the stop level.
    m.begin_evaluation_window();
    auto fires = m.on_bar("BTCUSDT", 97.0, 96.0, 97.2, 96.5, kT0);
    ASSERT_EQ(fires.size(), 1u);
    EXPECT_NEAR(fires[0].get_price(), 97.0, 1e-9)
        << "F-01(b) must not break real gap handling";
}

TEST(ForensicLifecycle, F01b_SyncFillArmedAfterEvaluationGapsNormallyNextWindow)
{
    ExitManager m;
    // execution_bar_delay == 0: the opener fills at the bar close, i.e. after
    // this window's evaluation already ran. The next bar's open is genuinely
    // after the fill, so a gap through the stop is real and must fill there.
    m.begin_evaluation_window();
    EXPECT_TRUE(m.on_bar("BTCUSDT", 100.0, 99.9, 100.1, 100.0, kT0).empty());
    m.register_pending(long_intent(1, 100.0, 99.0, 103.0));
    m.on_fill(opener_fill(1, order_side::buy, 1.0, 100.0));

    m.begin_evaluation_window();
    auto fires = m.on_bar("BTCUSDT", 97.0, 96.0, 97.2, 96.5, kT0);
    ASSERT_EQ(fires.size(), 1u);
    EXPECT_NEAR(fires[0].get_price(), 97.0, 1e-9)
        << "a bracket armed before this bar opened can be gapped through";
}

TEST(ForensicLifecycle, F01b_ShortMirrorDoesNotFillAtAPreArmOpen)
{
    ExitManager m;
    m.begin_evaluation_window();
    m.register_pending(short_intent(2, 100.0, 101.0, 97.0));
    m.on_fill(opener_fill(2, order_side::sell, 1.0, 99.50));
    ASSERT_EQ(m.armed_count(), 1u);
    EXPECT_NEAR(armed_stop(m, 2), 100.50, 1e-9);

    auto fires = m.on_bar("BTCUSDT", 101.0, 96.0, 104.0, 99.0, kT0);
    ASSERT_EQ(fires.size(), 1u);
    EXPECT_NEAR(fires[0].get_price(), 100.50, 1e-9)
        << "a short armed inside the bar must not exit at that bar's open";

}

TEST(ForensicLifecycle, F01b_TickPathIsUnaffectedByWindows)
{
    // on_price has no anchoring choice: px is a price the market is trading
    // at right now, so there is no gap branch and nothing to defer.
    ExitManager m;
    m.begin_evaluation_window();
    m.register_pending(long_intent(1, 100.0, 99.0, 103.0));
    m.on_fill(opener_fill(1, order_side::buy, 1.0, 100.0));
    auto fires = m.on_price("BTCUSDT", 98.0, kT0);
    ASSERT_EQ(fires.size(), 1u);
    EXPECT_NEAR(fires[0].get_price(), 98.0, 1e-9);
}

TEST(ForensicLifecycle, F01b_WithoutAnEngineWindowTheManagerKeepsPreFixAnchoring)
{
    // Embedders that never open a window (unit tools) keep the pre-fix
    // behaviour rather than silently changing every anchored fill price.
    ExitManager m;
    m.register_pending(long_intent(1, 100.0, 99.0, 103.0));
    m.on_fill(opener_fill(1, order_side::buy, 1.0, 100.0));
    auto fires = m.on_bar("BTCUSDT", 98.0, 97.0, 100.5, 97.5, kT0);
    ASSERT_EQ(fires.size(), 1u);
    EXPECT_NEAR(fires[0].get_price(), 98.0, 1e-9);
}


// ── F-03: opener fill precedes intent registration ─────────────────────────

TEST(ForensicLifecycle, F03_FillBeforeRegistrationStillArmsExactlyOnce)
{
    ExitManager m;
    m.begin_evaluation_window();
    // Synchronous fill inside route(): the fill lands before finalize_route
    // registers the intent. Pre-fix the intent sat in pending_ forever and
    // the position ran unprotected for the rest of the run.
    m.on_fill(opener_fill(9, order_side::buy, 1.0, 100.0));
    EXPECT_EQ(m.armed_count(), 0u);
    EXPECT_EQ(m.counters().orphan_fills_recorded, 1u);

    m.register_pending(long_intent(9, 100.0, 99.0, 103.0));
    EXPECT_EQ(m.armed_count(), 1u);
    EXPECT_EQ(m.pending_count(), 0u);
    EXPECT_EQ(m.counters().deferred_arms, 1u);
    EXPECT_NEAR(armed_entry(m, 9), 100.0, 1e-12);
}

TEST(ForensicLifecycle, F03_BothOrderingsProduceExactlyOneArmedIntent)
{
    for (bool fill_first : {false, true})
    {
        ExitManager m;
        m.begin_evaluation_window();
        if (fill_first)
        {
            m.on_fill(opener_fill(4, order_side::buy, 2.0, 100.0));
            m.register_pending(long_intent(4, 100.0, 99.0, 103.0, 2.0));
        }
        else
        {
            m.register_pending(long_intent(4, 100.0, 99.0, 103.0, 2.0));
            m.on_fill(opener_fill(4, order_side::buy, 2.0, 100.0));
        }
        EXPECT_EQ(m.armed_count(), 1u) << "fill_first=" << fill_first;
        EXPECT_EQ(m.pending_count(), 0u) << "fill_first=" << fill_first;

        m.begin_evaluation_window();
        EXPECT_EQ(m.on_bar("BTCUSDT", 100.0, 98.0, 100.2, 98.5, kT0).size(), 1u)
            << "the deferred arm must be a live bracket, fill_first="
            << fill_first;

    }
}

TEST(ForensicLifecycle, F03_DeferredArmStillHonoursTheSlippageRefusal)
{
    ExitManager m;
    m.begin_evaluation_window();
    m.on_fill(opener_fill(11, order_side::buy, 1.0, 101.5));
    m.register_pending(long_intent(11, 100.0, 99.0, 103.0));

    EXPECT_EQ(m.armed_count(), 0u);
    EXPECT_EQ(m.counters().slippage_disarms, 1u);
    EXPECT_EQ(m.take_flatten_requests().size(), 1u);
}

TEST(ForensicLifecycle, F03_SiblingIntentsBothArmFromOneDeferredFill)
{
    ExitManager m;
    m.begin_evaluation_window();
    m.on_fill(opener_fill(12, order_side::buy, 4.0, 100.0));

    auto tp1 = long_intent(12, 100.0, 99.0, 101.0, 4.0);
    tp1.qty_fraction = 0.5;
    auto runner = long_intent(12, 100.0, 99.0, 105.0, 4.0);
    runner.qty_fraction = 0.5;
    m.register_pending(tp1);
    m.register_pending(runner);

    EXPECT_EQ(m.armed_count(), 2u) << "scale-outs must both arm from one fill";
    EXPECT_EQ(m.counters().deferred_arms, 2u);
}

TEST(ForensicLifecycle, F03_OrphanFillExpiresAndCannotArmALaterIntent)
{
    ExitManager m;
    m.begin_evaluation_window();
    // An opener that simply carries no bracket at all.
    m.on_fill(opener_fill(21, order_side::buy, 1.0, 100.0));
    EXPECT_EQ(m.counters().orphan_fills_recorded, 1u);

    m.begin_evaluation_window();
    m.begin_evaluation_window();
    m.begin_evaluation_window();
    EXPECT_GE(m.counters().orphan_fills_evicted, 1u);

    // A much later intent that happens to key on the same id must NOT arm
    // from that stale fill — it goes to pending_ and waits for its own.
    m.register_pending(long_intent(21, 100.0, 99.0, 103.0));
    EXPECT_EQ(m.armed_count(), 0u);
    EXPECT_EQ(m.pending_count(), 1u);
}

// ── F-06: pending intents leak, unbounded ──────────────────────────────────

TEST(ForensicLifecycle, F06_PendingIntentsAreBoundedAndCounted)
{
    ExitManager m;
    // Every one of these openers dies without a terminal notification —
    // exactly what a rejected bar-delayed order does today (F-02).
    constexpr std::size_t kOver = 4096 + 32;
    for (std::size_t i = 1; i <= kOver; ++i)
        m.register_pending(long_intent(static_cast<std::uint64_t>(i),
                                       100.0, 99.0, 103.0));

    EXPECT_LE(m.pending_count(), 4096u) << "pending_ must not grow unbounded";
    EXPECT_EQ(m.counters().pending_evicted, kOver - 4096u);
    EXPECT_EQ(m.counters().pending_registered, kOver);
}

TEST(ForensicLifecycle, F06_LifecycleCountersTrackRegisterArmAndCancel)
{
    ExitManager m;
    m.register_pending(long_intent(1, 100.0, 99.0, 103.0));
    m.register_pending(long_intent(2, 100.0, 99.0, 103.0));
    m.on_fill(opener_fill(1, order_side::buy, 1.0, 100.0));
    m.cancel(std::uint64_t{2});

    EXPECT_EQ(m.counters().pending_registered, 2u);
    EXPECT_EQ(m.counters().armed, 1u);
    EXPECT_EQ(m.counters().cancelled, 1u);
    EXPECT_EQ(m.counters().pending_evicted, 0u);
}

// ── F-09a: multi-level opener fills strand SL/TP on the first partial ──────

TEST(ForensicLifecycle, F09a_MultiLevelOpenerWalkKeepsTheDesignedStopDistance)
{
    ExitManager m;
    m.begin_evaluation_window();
    // Designed risk 5.00. The opener walks four book levels upward; pre-fix
    // entry_price rolled to the VWAP while stop_loss stayed on the first
    // partial, turning a 5.00 designed distance into a much wider real one.
    m.register_pending(long_intent(1, /*ref=*/7000.0, /*sl=*/6995.0,
                                   /*tp=*/7020.0, 4.0));
    m.on_fill(opener_fill(1, order_side::buy, 1.0, 7000.40));
    m.on_fill(opener_fill(1, order_side::buy, 1.0, 7000.80));
    m.on_fill(opener_fill(1, order_side::buy, 1.0, 7001.20));
    m.on_fill(opener_fill(1, order_side::buy, 1.0, 7001.60));

    ASSERT_EQ(m.armed_count(), 1u);
    const double entry = armed_entry(m, 1);
    EXPECT_NEAR(entry, 7001.00, 1e-9) << "entry rolls to the VWAP of all legs";
    EXPECT_NEAR(entry - armed_stop(m, 1), 5.00, 1e-9)
        << "the designed risk distance must survive the level walk";
}

TEST(ForensicLifecycle, F09a_ArmedQuantityStillCoversEveryLeg)
{
    ExitManager m;
    m.begin_evaluation_window();
    m.register_pending(long_intent(1, 7000.0, 6995.0, 7020.0, 4.0));
    for (int i = 0; i < 4; ++i)
        m.on_fill(opener_fill(1, order_side::buy, 1.0, 7000.0 + 0.4 * i));

    m.begin_evaluation_window();
    auto fires = m.on_bar("BTCUSDT", 7001.0, 6990.0, 7002.0, 6991.0, kT0);
    ASSERT_EQ(fires.size(), 1u);
    EXPECT_NEAR(fires[0].get_quantity(), 4.0, 1e-9)
        << "the exit must cover the whole walked position";
}
