// Pins the engine-side SL/TP/trailing enforcement module.
// The ExitManager owns stop-loss / take-profit / trailing / time exits
// for every strategy: strategies declare intent, the manager arms on
// opener fill, and returns a synthetic close order on trigger. These
// tests lock behavior across the full lifecycle so that downstream
// changes (different strategies, different engine paths) keep a single
// enforcement semantics.

#include <gtest/gtest.h>
#include "exits/exit_manager.h"
#include "core/event.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

using namespace truetest::exits;
using tp = std::chrono::system_clock::time_point;

namespace {

tp t0 = std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}};

exit_intent make_long_intent(const std::string& strat, const std::string& sym,
                             std::uint64_t opener_id,
                             std::optional<double> sl,
                             std::optional<double> tp,
                             double qty = 1.0)
{
    exit_intent ei;
    ei.strategy_name    = strat;
    ei.symbol           = sym;
    ei.close_side       = order_side::sell;
    ei.qty              = qty;
    ei.stop_loss        = sl;
    ei.take_profit      = tp;
    ei.opener_order_id  = opener_id;
    return ei;
}

fill_event make_opener_fill(std::uint64_t id, const std::string& sym,
                            order_side side, double qty, double price,
                            tp ts = t0)
{
    return fill_event(ts, sym, id, side, qty, price, /*commission=*/0.0,
                      /*remaining=*/0.0, /*fill_id=*/1);
}

}

TEST(ExitManager, PendingOnlyFiresAfterOpenerFill)
{
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 42, /*sl=*/90.0, /*tp=*/110.0));

    // Before the opener fills, no price event can trigger - the intent
    // is pending, not armed.
    auto r = m.on_price("X", 80.0, t0);
    EXPECT_TRUE(r.empty());
    EXPECT_EQ(m.pending_count(), 1u);
    EXPECT_EQ(m.armed_count(), 0u);

    m.on_fill(make_opener_fill(42, "X", order_side::buy, 1.0, 100.0));
    EXPECT_EQ(m.pending_count(), 0u);
    EXPECT_EQ(m.armed_count(), 1u);
}

TEST(ExitManager, StopLossFiresAtOrBelowThresholdAndDisarms)
{
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, /*sl=*/95.0, /*tp=*/110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 2.0, 100.0));

    // Above SL - no fire.
    EXPECT_TRUE(m.on_price("X", 96.0, t0).empty());

    // At SL - fire, market sell, correct qty.
    auto r = m.on_price("X", 95.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r[0].get_symbol(), "X");
    EXPECT_EQ(r[0].get_side(), order_side::sell);
    EXPECT_EQ(r[0].get_order_type(), order_type::market);
    EXPECT_DOUBLE_EQ(r[0].get_quantity(), 2.0);  // from opener fill qty, not intent qty
    // Second tick at the same price shouldn't re-fire - the intent is
    // gone after trigger.
    EXPECT_TRUE(m.on_price("X", 95.0, t0).empty());
    EXPECT_EQ(m.armed_count(), 0u);
}

TEST(ExitManager, TakeProfitFiresAtOrAboveThreshold)
{
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, /*sl=*/90.0, /*tp=*/110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    EXPECT_TRUE(m.on_price("X", 109.99, t0).empty());
    auto r = m.on_price("X", 110.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r[0].get_side(), order_side::sell);
}

TEST(ExitManager, GapPastSlFillsAtNextObservedPriceNotAtSl)
{
    // Realistic shadow: if the next tick prints below SL, we fire at
    // that tick's price. The engine then submits a market order; the
    // shadow adapter fills at the tape. The manager itself must emit
    // the observed price, not the SL level.
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, /*sl=*/95.0, /*tp=*/110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    auto r = m.on_price("X", 91.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_DOUBLE_EQ(r[0].get_price(), 91.0);
}

TEST(ExitManager, TrailingStopRaisesSlOnFavorableTicks)
{
    // 1% trailing: entry 100, best=105 -> trailed SL = 103.95.
    // Then price drops to 103.9 -> SL fires at 103.9, not original 95.
    ExitManager m;
    exit_intent ei = make_long_intent("s", "X", 1, /*sl=*/95.0, std::nullopt);
    ei.trailing_pct = 0.01;
    m.register_pending(std::move(ei));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    EXPECT_TRUE(m.on_price("X", 103.0, t0).empty());  // trail raises SL to 101.97
    EXPECT_TRUE(m.on_price("X", 105.0, t0).empty());  // trail raises SL to 103.95
    EXPECT_TRUE(m.on_price("X", 104.0, t0).empty());  // above trailed SL
    auto r = m.on_price("X", 103.90, t0);
    ASSERT_FALSE(r.empty());
}

TEST(ExitManager, TimeStopFiresAfterDeadline)
{
    ExitManager m;
    exit_intent ei = make_long_intent("s", "X", 1, std::nullopt, std::nullopt);
    ei.deadline = t0 + std::chrono::seconds(5);
    m.register_pending(std::move(ei));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    EXPECT_TRUE(m.on_price("X", 100.0, t0 + std::chrono::seconds(4)).empty());
    auto r = m.on_price("X", 100.0, t0 + std::chrono::seconds(5));
    ASSERT_FALSE(r.empty());
}

TEST(ExitManager, CancelDropsPendingAndArmed)
{
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, 95.0, 110.0));
    m.cancel("s", "X");
    EXPECT_EQ(m.pending_count(), 0u);

    m.register_pending(make_long_intent("s", "Y", 2, 95.0, 110.0));
    m.on_fill(make_opener_fill(2, "Y", order_side::buy, 1.0, 100.0));
    m.cancel("s", "Y");
    EXPECT_EQ(m.armed_count(), 0u);
    EXPECT_TRUE(m.on_price("Y", 95.0, t0).empty());
}

TEST(ExitManager, FillBindsToActualOpenerPriceAndQty)
{
    // The intent's qty was a hint; the opener may have filled partially
    // or at a different price (market orders against the tape). The
    // manager uses the actual fill price as entry reference for future
    // trailing updates and the actual fill qty as the close qty.
    ExitManager m;
    exit_intent ei = make_long_intent("s", "X", 7, /*sl=*/99.0, std::nullopt, /*qty=*/1.0);
    ei.trailing_pct = 0.05;
    m.register_pending(std::move(ei));
    // Opener filled 0.6 @ 102 (partial, different price from intended 100).
    m.on_fill(make_opener_fill(7, "X", order_side::buy, 0.6, 102.0));

    auto r = m.on_price("X", 96.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_DOUBLE_EQ(r[0].get_quantity(), 0.6);
}

TEST(ExitManager, EntryRelativeBracketsRebaseOnFillSlippage)
{
    // Intended entry 100, SL 98 ($2 risk), TP 106. Fill at 100.5 must
    // shift both levels by +0.5 → SL 98.5, TP 106.5.
    ExitManager m;
    exit_intent ei = make_long_intent("s", "X", 1, /*sl=*/98.0, /*tp=*/106.0, /*qty=*/1.0);
    ei.reference_entry = 100.0;
    m.register_pending(std::move(ei));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.5));

    // Above rebased SL (98.5): still armed.
    auto miss = m.on_price("X", 98.6, t0);
    EXPECT_TRUE(miss.empty());
    EXPECT_EQ(m.armed_count(), 1u);

    // At rebased SL: long fires when px <= stop.
    // Without rebase this would still be above the old SL of 98.0.
    auto hit = m.on_price("X", 98.5, t0);
    ASSERT_FALSE(hit.empty());
    EXPECT_EQ(m.armed_count(), 0u);
}

TEST(ExitManager, AbsoluteStructureStopDoesNotRebaseWithoutReference)
{
    // Absolute structure SL stays put when reference_entry is unset —
    // used for consol-low / swing stops that are market levels.
    ExitManager m;
    exit_intent ei = make_long_intent("s", "X", 1, /*sl=*/98.0, std::nullopt, /*qty=*/1.0);
    // reference_entry deliberately left unset
    m.register_pending(std::move(ei));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.5));

    auto hit = m.on_price("X", 98.0, t0);
    ASSERT_FALSE(hit.empty());
}

TEST(ExitManager, MultipleStrategiesOnSameSymbolKeyIndependently)
{
    // Two strategies both long X with different stops; canceling one
    // leaves the other intact.
    ExitManager m;
    m.register_pending(make_long_intent("a", "X", 1, 95.0, 110.0));
    m.register_pending(make_long_intent("b", "X", 2, 90.0, 120.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));
    m.on_fill(make_opener_fill(2, "X", order_side::buy, 1.0, 100.0));
    EXPECT_EQ(m.armed_count(), 2u);

    // 93: between a.sl and b.sl - only a should fire.
    auto r = m.on_price("X", 93.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(m.armed_count(), 1u);

    // Tick at 89 triggers b.
    auto r2 = m.on_price("X", 89.0, t0);
    ASSERT_FALSE(r2.empty());
    EXPECT_EQ(m.armed_count(), 0u);
}

// ---- Partial exits (Phase C) ------------------------------------------

TEST(ExitManager, PartialExit_SingleIntent_ClosesFraction)
{
    // Opener for 10 units; intent with qty_fraction = 0.5 ⇒ close_qty = 5.
    ExitManager m;
    auto intent = make_long_intent("s", "X", 1, /*sl=*/90.0, /*tp=*/110.0, /*qty=*/10.0);
    intent.qty_fraction = 0.5;
    m.register_pending(std::move(intent));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, /*qty=*/10.0, 100.0));

    auto r = m.on_price("X", 89.0, t0);    // SL crossed
    ASSERT_FALSE(r.empty());
    EXPECT_NEAR(r[0].get_quantity(), 5.0, 1e-9);
}

TEST(ExitManager, PartialExit_MultipleIntentsPerKey_ArmAll)
{
    // Strategy declares TP1 = 0.5 @ 110, TP2 = 0.3 @ 120, SL = 1.0 @ 90.
    ExitManager m;
    auto make = [](double frac, std::optional<double> sl, std::optional<double> tp) {
        auto i = make_long_intent("s", "X", 42, sl, tp, /*qty=*/10.0);
        i.qty_fraction = frac;
        return i;
    };
    m.register_pending(make(0.5, std::nullopt, 110.0));
    m.register_pending(make(0.3, std::nullopt, 120.0));
    m.register_pending(make(1.0, 90.0, std::nullopt));

    EXPECT_EQ(m.pending_count(), 3u);
    m.on_fill(make_opener_fill(42, "X", order_side::buy, 10.0, 100.0));
    EXPECT_EQ(m.pending_count(), 0u);
    EXPECT_EQ(m.armed_count(), 3u);
}

TEST(ExitManager, PartialExit_TP1FiresWithoutAffectingTP2OrSL)
{
    ExitManager m;
    auto make = [](double frac, std::optional<double> sl, std::optional<double> tp) {
        auto i = make_long_intent("s", "X", 42, sl, tp, 10.0);
        i.qty_fraction = frac;
        return i;
    };
    m.register_pending(make(0.5, std::nullopt, 110.0));   // TP1
    m.register_pending(make(0.3, std::nullopt, 120.0));   // TP2
    m.register_pending(make(1.0, 90.0, std::nullopt));    // SL
    m.on_fill(make_opener_fill(42, "X", order_side::buy, 10.0, 100.0));

    // Touch 110 - TP1 fires, closing 5 units. TP2 and SL remain armed.
    auto r1 = m.on_price("X", 110.0, t0);
    ASSERT_FALSE(r1.empty());
    EXPECT_NEAR(r1[0].get_quantity(), 5.0, 1e-9);
    EXPECT_EQ(m.armed_count(), 2u);

    fill_event tp1_fill(t0, "X", 900, order_side::sell, 5.0, 110.0,
                        /*commission=*/0.0, /*remaining=*/0.0,
                        /*fill_id=*/1, "s", 42);
    m.on_fill(tp1_fill, 42);
    EXPECT_EQ(m.armed_count(), 2u);

    // Touch 120 - TP2 fires, closing 3 units. SL remains.
    auto r2 = m.on_price("X", 120.0, t0);
    ASSERT_FALSE(r2.empty());
    EXPECT_NEAR(r2[0].get_quantity(), 3.0, 1e-9);
    EXPECT_EQ(m.armed_count(), 1u);

    // Crash to 89 - SL can only close the remaining 2 units after TP1 and TP2.
    auto r3 = m.on_price("X", 89.0, t0);
    ASSERT_FALSE(r3.empty());
    EXPECT_NEAR(r3[0].get_quantity(), 2.0, 1e-9);
    EXPECT_EQ(m.armed_count(), 0u);
}

TEST(ExitManager, PartialExit_DefaultFractionIsFullClose)
{
    // No qty_fraction set -> default 1.0 -> close_qty = opener_qty.
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, 90.0, 110.0, /*qty=*/10.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 10.0, 100.0));

    auto r = m.on_price("X", 89.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_NEAR(r[0].get_quantity(), 10.0, 1e-9);
}

TEST(ExitManager, PartialExit_ClampsNegativeAndOutOfRangeFractions)
{
    ExitManager m;
    auto bad = make_long_intent("s", "X", 1, 90.0, 110.0, 10.0);
    bad.qty_fraction = -0.5;                   // nonsensical -> treated as 1.0
    m.register_pending(std::move(bad));

    auto too_big = make_long_intent("s", "Y", 2, 90.0, 110.0, 10.0);
    too_big.qty_fraction = 1.5;                // > 1.0 -> clamped to 1.0
    m.register_pending(std::move(too_big));

    m.on_fill(make_opener_fill(1, "X", order_side::buy, 10.0, 100.0));
    m.on_fill(make_opener_fill(2, "Y", order_side::buy, 10.0, 100.0));

    auto r1 = m.on_price("X", 89.0, t0);
    ASSERT_FALSE(r1.empty());
    EXPECT_NEAR(r1[0].get_quantity(), 10.0, 1e-9);   // clamped to full

    auto r2 = m.on_price("Y", 89.0, t0);
    ASSERT_FALSE(r2.empty());
    EXPECT_NEAR(r2[0].get_quantity(), 10.0, 1e-9);   // clamped to full
}

TEST(ExitManager, PartialExit_CancelRemovesAllIntentsForKey)
{
    ExitManager m;
    auto make = [](double frac, std::optional<double> sl, std::optional<double> tp) {
        auto i = make_long_intent("s", "X", 42, sl, tp, 10.0);
        i.qty_fraction = frac;
        return i;
    };
    m.register_pending(make(0.5, std::nullopt, 110.0));
    m.register_pending(make(1.0, 90.0, std::nullopt));
    m.on_fill(make_opener_fill(42, "X", order_side::buy, 10.0, 100.0));
    EXPECT_EQ(m.armed_count(), 2u);

    m.cancel("s", "X");
    EXPECT_EQ(m.armed_count(), 0u);
    EXPECT_EQ(m.pending_count(), 0u);
}

// ---- Bar variant (low/high probing) -----------------------------------

TEST(ExitManager, OnBar_LongSlFiresOnLowEvenWhenCloseRecovers)
{
    // Bar wicks below SL but closes above - on_price(close) would miss it,
    // on_bar must catch it.
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, /*sl=*/95.0, /*tp=*/110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    auto r = m.on_bar("X", /*open=*/100.0, /*low=*/94.0, /*high=*/101.0,
                      /*close=*/100.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r[0].get_side(), order_side::sell);
    EXPECT_DOUBLE_EQ(r[0].get_price(), 95.0);  // anchored at the SL level
    EXPECT_EQ(m.armed_count(), 0u);
}

TEST(ExitManager, OnBar_LongSlGapThroughFiresAtOpen)
{
    // Bar opens below the SL: the stop couldn't fill at its level — it
    // fills at the (worse) open.
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, /*sl=*/95.0, /*tp=*/110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    auto r = m.on_bar("X", /*open=*/92.0, /*low=*/91.0, /*high=*/96.0,
                      /*close=*/93.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_DOUBLE_EQ(r[0].get_price(), 92.0);  // gap-through → open
}

TEST(ExitManager, OnBar_LongTpFiresOnHighEvenWhenCloseDoesntReach)
{
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, 95.0, 110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    auto r = m.on_bar("X", /*open=*/100.0, 99.0, 112.0, 105.0, t0);
    ASSERT_FALSE(r.empty());
    // A TP is a resting limit: it fills at the TP level, not the extreme.
    EXPECT_DOUBLE_EQ(r[0].get_price(), 110.0);
}

TEST(ExitManager, OnBar_BothTouchedSlWinsForLong)
{
    // Conservative: when both extremes cross in one bar, SL fires (worst).
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, 95.0, 110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    auto r = m.on_bar("X", /*open=*/100.0, 94.0, 111.0, 100.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_DOUBLE_EQ(r[0].get_price(), 95.0);  // SL level, not TP
}

TEST(ExitManager, OnBar_ShortSlFiresOnHigh)
{
    // For shorts the worst case is the high.
    ExitManager m;
    exit_intent ei;
    ei.symbol           = "X";
    ei.close_side       = order_side::buy;
    ei.qty              = 1.0;
    ei.stop_loss        = 105.0;
    ei.take_profit      = 95.0;
    ei.opener_order_id  = 1;
    ei.strategy_name    = "s";
    m.register_pending(std::move(ei));
    m.on_fill(make_opener_fill(1, "X", order_side::sell, 1.0, 100.0));

    auto r = m.on_bar("X", /*open=*/100.0, /*low=*/96.0, /*high=*/106.0,
                      /*close=*/100.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r[0].get_side(), order_side::buy);
    EXPECT_DOUBLE_EQ(r[0].get_price(), 105.0);  // anchored at the SL level
}

TEST(ExitManager, OnBar_NoTriggerLeavesIntentArmed)
{
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, 95.0, 110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    EXPECT_TRUE(m.on_bar("X", /*open=*/100.0, 96.0, 109.0, 100.0, t0).empty());
    EXPECT_EQ(m.armed_count(), 1u);
}

TEST(ExitManager, OnBar_TrailingStopUsesPreBarLevel)
{
    // The trail must be tested at its level from PREVIOUS bars. Raising it
    // with this bar's high and then testing this bar's low would assume the
    // high printed first (intra-bar look-ahead).
    ExitManager m;
    auto i = make_long_intent("s", "X", 1, std::nullopt, std::nullopt);
    i.trailing_pct = 0.05;  // 5% trail
    m.register_pending(std::move(i));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    // Bar 1: high 120 → trail becomes 114 for NEXT bars; low 110 must be
    // tested against the (not yet existing) pre-bar trail → no fire.
    EXPECT_TRUE(m.on_bar("X", 100.0, 110.0, 120.0, 118.0, t0).empty());
    EXPECT_EQ(m.armed_count(), 1u);

    // Bar 2: low 113 crosses the 114 trail from bar 1 → fires at 114.
    auto r = m.on_bar("X", 118.0, 113.0, 119.0, 116.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_DOUBLE_EQ(r[0].get_price(), 114.0);
}

TEST(ExitManager, PartialOpenerFills_GrowArmedQtyAndVwapEntry)
{
    // The book can emit one fill per walked level for a single opener.
    // The armed bracket must cover the full position, not just the first
    // partial — and the entry reference rolls to the VWAP.
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, 95.0, 110.0, /*qty=*/0.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 4.0, 100.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 6.0, 101.0));
    EXPECT_EQ(m.armed_count(), 1u);

    auto r = m.on_bar("X", 100.0, 94.0, 101.0, 100.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_DOUBLE_EQ(r[0].get_quantity(), 10.0);  // 4 + 6, not 4

    auto snap = m.snapshot_armed();
    EXPECT_TRUE(snap.empty());  // fired and erased
}

// ---- Bracket adapter integration ---------------------------------------

namespace {

struct FakeAdapter : public IBracketAdapter
{
    bracket_caps caps_state{};
    int place_calls  = 0;
    int cancel_calls = 0;
    std::vector<std::uint64_t> placed_openers;
    std::vector<std::uint64_t> cancelled_openers;
    bracket_handles next_handles;
    bool throw_cancel = false;

    bracket_caps capabilities() const override { return caps_state; }

    bracket_handles place(std::uint64_t opener,
                          const exit_intent&,
                          double) override
    {
        ++place_calls;
        placed_openers.push_back(opener);
        return next_handles;
    }

    void cancel(std::uint64_t opener,
                const bracket_handles&) override
    {
        ++cancel_calls;
        cancelled_openers.push_back(opener);
        if (throw_cancel) throw std::runtime_error("ambiguous cancel");
    }
};

native_bracket_update native_economic_update(
    native_bracket_lifecycle lifecycle,
    std::string_view exchange_id,
    std::string_view symbol,
    std::string_view group_id,
    std::string_view execution_id,
    double last_qty,
    double cumulative_qty,
    double price = 99.0,
    std::int64_t event_time_ms = 1'700'000'000'000,
    std::uint64_t source_sequence = 0)
{
    native_bracket_update update;
    update.lifecycle = lifecycle;
    update.exchange_order_id = exchange_id;
    update.client_order_id = "venue-client";
    update.execution_id = execution_id;
    update.symbol = symbol;
    update.group_id = group_id;
    update.side = order_side::sell;
    update.event_time_ms = event_time_ms;
    update.source_sequence = source_sequence != 0
        ? source_sequence : static_cast<std::uint64_t>(event_time_ms);
    update.last_fill_qty = last_qty;
    update.last_fill_price = price;
    update.cumulative_qty = cumulative_qty;
    update.cumulative_reported = true;
    return update;
}

native_bracket_update native_terminal_update(
    native_bracket_lifecycle lifecycle,
    std::string_view exchange_id,
    std::string_view symbol,
    std::string_view group_id,
    double cumulative_qty,
    std::int64_t event_time_ms = 1'700'000'000'100)
{
    native_bracket_update update;
    update.lifecycle = lifecycle;
    update.exchange_order_id = exchange_id;
    update.client_order_id = "venue-client";
    update.symbol = symbol;
    update.group_id = group_id;
    update.side = order_side::sell;
    update.event_time_ms = event_time_ms;
    update.cumulative_qty = cumulative_qty;
    update.cumulative_reported = true;
    return update;
}

}

TEST(ExitManagerAdapter, OpenerFillTriggersPlaceWithStableHandles)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->caps_state.oco = true;
    fake->next_handles.sl_exchange_id = "sl-1";
    fake->next_handles.tp_exchange_id = "tp-1";
    fake->next_handles.oco_list_id    = "list-A";
    m.set_bracket_adapter(fake);

    m.register_pending(make_long_intent("s", "X", 7, 95.0, 110.0));
    EXPECT_EQ(fake->place_calls, 0);

    m.on_fill(make_opener_fill(7, "X", order_side::buy, 1.0, 100.0));
    ASSERT_EQ(fake->place_calls, 1);
    EXPECT_EQ(fake->placed_openers[0], 7u);

    // Reverse map populated for both legs.
    EXPECT_EQ(m.opener_for_exchange_order("sl-1"), 7u);
    EXPECT_EQ(m.opener_for_exchange_order("tp-1"), 7u);
    EXPECT_EQ(m.opener_for_exchange_order("nope"), 0u);
}

TEST(ExitManagerAdapter, EmptyHandlesSkipsExchangeMap)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    // place returns empty handles -> adapter declined (e.g. only stop_market
    // available but intent had both SL+TP). Engine-side eval still runs.
    m.set_bracket_adapter(fake);

    m.register_pending(make_long_intent("s", "X", 1, 95.0, 110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    EXPECT_EQ(fake->place_calls, 1);
    EXPECT_EQ(m.opener_for_exchange_order("anything"), 0u);
}

TEST(ExitManagerAdapter, PriceTriggerCancelsVenueBracket)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-9";
    fake->next_handles.tp_exchange_id = "tp-9";
    m.set_bracket_adapter(fake);

    m.register_pending(make_long_intent("s", "X", 9, 95.0, 110.0));
    m.on_fill(make_opener_fill(9, "X", order_side::buy, 1.0, 100.0));
    EXPECT_EQ(fake->place_calls, 1);

    auto r = m.on_price("X", 94.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(fake->cancel_calls, 1);
    EXPECT_EQ(fake->cancelled_openers[0], 9u);

    // REST cancel is advisory.  The reverse identity remains until the
    // private terminal arrives, so a fill racing that cancel can still be
    // attributed and accounted instead of vanishing as an unknown order.
    EXPECT_EQ(m.opener_for_exchange_order("sl-9"), 9u);
}

TEST(ExitManagerAdapter, ManualCancelCancelsVenueBracket)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.oco_list_id = "list-77";
    m.set_bracket_adapter(fake);

    m.register_pending(make_long_intent("s", "X", 77, 95.0, 110.0));
    m.on_fill(make_opener_fill(77, "X", order_side::buy, 1.0, 100.0));
    m.cancel(77u);
    EXPECT_EQ(fake->cancel_calls, 1);
}

TEST(ExitManagerAdapter, FailedVenueCancelRestoresHandlesAndReverseIdentity)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-88";
    fake->next_handles.tp_exchange_id = "tp-88";
    fake->throw_cancel = true;
    m.set_bracket_adapter(fake);

    m.register_pending(make_long_intent("s", "X", 88, 95.0, 110.0));
    m.on_fill(make_opener_fill(88, "X", order_side::buy, 1.0, 100.0));
    ASSERT_EQ(m.opener_for_exchange_order("sl-88"), 88u);
    ASSERT_EQ(m.strategy_name_for_exchange_order("sl-88"), "s");

    EXPECT_THROW(m.cancel(88u), std::runtime_error);
    EXPECT_EQ(m.opener_for_exchange_order("sl-88"), 88u);
    EXPECT_EQ(m.strategy_name_for_exchange_order("sl-88"), "s");

    fake->throw_cancel = false;
    EXPECT_NO_THROW(m.cancel(88u));
    EXPECT_EQ(fake->cancel_calls, 2);
    EXPECT_EQ(m.opener_for_exchange_order("sl-88"), 88u);
}

TEST(ExitManagerAdapter, BulkCancelByStrategySymbolCancelsAllVenueBrackets)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.oco_list_id = "list";
    m.set_bracket_adapter(fake);

    m.register_pending(make_long_intent("s", "X", 1, 95.0, 110.0));
    m.register_pending(make_long_intent("s", "X", 2, 95.0, 110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));
    m.on_fill(make_opener_fill(2, "X", order_side::buy, 1.0, 100.0));

    m.cancel("s", "X");
    EXPECT_EQ(fake->cancel_calls, 2);
}

TEST(ExitManagerAdapter, RehydrateInstallsArmedAndVenueState)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    m.set_bracket_adapter(fake);

    IBracketAdapter::recovered_bracket rb;
    rb.opener_order_id = 555;
    rb.strategy_name   = "mr";
    rb.symbol          = "X";
    rb.close_side      = order_side::sell;
    rb.qty             = 1.0;
    rb.entry_price     = 100.0;
    rb.stop_loss       = 95.0;
    rb.take_profit     = 110.0;
    rb.handles.sl_exchange_id = "sl-r";
    rb.handles.tp_exchange_id = "tp-r";
    rb.handles.oco_list_id    = "list-r";

    m.rehydrate(rb);

    EXPECT_EQ(m.armed_count(), 1u);
    EXPECT_EQ(m.openers_for("mr", "X"), 1u);
    EXPECT_EQ(m.opener_for_exchange_order("sl-r"), 555u);
    EXPECT_EQ(m.opener_for_exchange_order("tp-r"), 555u);

    // Now a price tick crosses SL -> ExitManager fires AND cancels
    // venue-side via the adapter (defense in depth: even if Binance
    // already filled the OCO, our DELETE is idempotent).
    auto r = m.on_price("X", 94.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(fake->cancel_calls, 1);
    EXPECT_EQ(fake->cancelled_openers[0], 555u);
}

TEST(ExitManagerAdapter, RehydrateRejectsEmptyHandlesOrZeroOpener)
{
    ExitManager m;
    IBracketAdapter::recovered_bracket bad;
    bad.opener_order_id = 0;          // missing opener
    bad.symbol          = "X";
    bad.handles.oco_list_id = "g";
    m.rehydrate(bad);
    EXPECT_EQ(m.armed_count(), 0u);

    bad.opener_order_id = 1;
    bad.handles = {};                  // empty handles -> adapter declined
    m.rehydrate(bad);
    EXPECT_EQ(m.armed_count(), 0u);
}

TEST(ExitManagerAdapter, RehydrateRefusesZeroQuantityVenueState)
{
    ExitManager m;
    IBracketAdapter::recovered_bracket rb;
    rb.opener_order_id = 7;
    rb.symbol = "X";
    rb.qty = 0.0;
    rb.handles.sl_exchange_id = "sl";
    EXPECT_THROW(m.rehydrate(rb), std::runtime_error);
    EXPECT_EQ(m.armed_count(), 0u);
}

TEST(ExitManagerNativeBracket, ExactLegIdentityAndEconomicReplayAreStrict)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-native";
    fake->next_handles.tp_exchange_id = "tp-native";
    fake->next_handles.oco_list_id = "oco-native";
    fake->next_handles.symbol = "BTCUSDT";
    m.set_bracket_adapter(fake);
    m.register_pending(make_long_intent("mr", "BTCUSDT", 71, 95.0, 110.0));
    m.on_fill(make_opener_fill(71, "BTCUSDT", order_side::buy, 1.0, 100.0));

    const auto fill = native_economic_update(
        native_bracket_lifecycle::full_fill, "sl-native", "BTCUSDT",
        "oco-native", "trade-71", 1.0, 1.0);
    const auto accepted = m.resolve_native_bracket_update(fill);
    EXPECT_EQ(accepted.kind, native_bracket_resolution_kind::economic_fill);
    EXPECT_EQ(accepted.opener_order_id, 71u);
    EXPECT_EQ(accepted.role, native_bracket_leg_role::stop_loss);
    EXPECT_EQ(accepted.close_side, order_side::sell);
    EXPECT_EQ(accepted.remaining_qty, 0.0);
    EXPECT_TRUE(accepted.terminal);
    EXPECT_EQ(accepted.strategy_name, "mr");
    EXPECT_TRUE(accepted.reservation.valid());
    EXPECT_TRUE(m.native_bracket_blocks_symbol_admission("btcusdt"));

    const auto replay_while_reserved = m.resolve_native_bracket_update(fill);
    EXPECT_EQ(replay_while_reserved.kind,
              native_bracket_resolution_kind::economic_fill);
    EXPECT_EQ(replay_while_reserved.reservation.source_sequence,
              accepted.reservation.source_sequence);
    EXPECT_EQ(replay_while_reserved.reservation.leg_token,
              accepted.reservation.leg_token);
    ASSERT_TRUE(m.commit_native_bracket_economic(accepted.reservation));

    EXPECT_EQ(m.resolve_native_bracket_update(fill).kind,
              native_bracket_resolution_kind::duplicate);

    auto contradictory_replay = fill;
    contradictory_replay.last_fill_price = 98.0;
    EXPECT_EQ(m.resolve_native_bracket_update(contradictory_replay).kind,
              native_bracket_resolution_kind::fatal);
    EXPECT_TRUE(m.native_bracket_requires_reconciliation());
}

TEST(ExitManagerNativeBracket,
     EconomicPreflightRollsBackWithoutConsumingTheVenueFact)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-rollback";
    m.set_bracket_adapter(fake);
    m.register_pending(make_long_intent("mr", "BTCUSDT", 81, 95.0, 110.0));
    m.on_fill(make_opener_fill(81, "BTCUSDT", order_side::buy, 1.0, 100.0));

    const auto fill = native_economic_update(
        native_bracket_lifecycle::full_fill, "sl-rollback", "BTCUSDT", "",
        "trade-81", 1.0, 1.0, 99.0, 1'700'000'000'181, 8'101);
    const auto first = m.resolve_native_bracket_update(fill);
    ASSERT_EQ(first.kind, native_bracket_resolution_kind::economic_fill);
    ASSERT_TRUE(first.reservation.valid());

    // This models a throw in pool/accounting/audit after preflight but before
    // native economic state becomes committed.
    ASSERT_TRUE(m.rollback_native_bracket_economic(first.reservation));
    const auto retried = m.resolve_native_bracket_update(fill);
    ASSERT_EQ(retried.kind, native_bracket_resolution_kind::economic_fill);
    EXPECT_EQ(retried.reservation.source_sequence,
              first.reservation.source_sequence);
    EXPECT_EQ(retried.reservation.leg_token, first.reservation.leg_token);
    ASSERT_TRUE(m.commit_native_bracket_economic(retried.reservation));
    EXPECT_EQ(m.resolve_native_bracket_update(fill).kind,
              native_bracket_resolution_kind::duplicate);
}

TEST(ExitManagerNativeBracket, ReservationTokenMatchesBothLegAndSequence)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-token-one";
    m.set_bracket_adapter(fake);
    m.register_pending(make_long_intent("mr", "BTCUSDT", 82, 95.0, 110.0));
    m.on_fill(make_opener_fill(82, "BTCUSDT", order_side::buy, 1.0, 100.0));

    fake->next_handles = {};
    fake->next_handles.sl_exchange_id = "sl-token-two";
    m.register_pending(make_long_intent("mr", "BTCUSDT", 83, 95.0, 110.0));
    m.on_fill(make_opener_fill(83, "BTCUSDT", order_side::buy, 1.0, 100.0));

    const auto one_fill = native_economic_update(
        native_bracket_lifecycle::full_fill, "sl-token-one", "BTCUSDT", "",
        "trade-82", 1.0, 1.0, 99.0, 1'700'000'000'182, 8'201);
    const auto two_fill = native_economic_update(
        native_bracket_lifecycle::full_fill, "sl-token-two", "BTCUSDT", "",
        "trade-83", 1.0, 1.0, 99.0, 1'700'000'000'183, 8'301);
    const auto one = m.resolve_native_bracket_update(one_fill);
    const auto two = m.resolve_native_bracket_update(two_fill);
    ASSERT_EQ(one.kind, native_bracket_resolution_kind::economic_fill);
    ASSERT_EQ(two.kind, native_bracket_resolution_kind::economic_fill);
    ASSERT_NE(one.reservation.leg_token, two.reservation.leg_token);

    auto mismatched = one.reservation;
    mismatched.leg_token = two.reservation.leg_token;
    EXPECT_FALSE(m.rollback_native_bracket_economic(mismatched));
    // A bad composite token must not consume another leg's reservation.
    EXPECT_TRUE(m.rollback_native_bracket_economic(two.reservation));
    EXPECT_TRUE(m.rollback_native_bracket_economic(one.reservation));
    EXPECT_TRUE(m.native_bracket_requires_reconciliation());
}

TEST(ExitManagerNativeBracket,
     DuplicatePendingSourceSequenceAcrossLegsFailsClosed)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-sequence-one";
    m.set_bracket_adapter(fake);
    m.register_pending(make_long_intent("mr", "BTCUSDT", 87, 95.0, 110.0));
    m.on_fill(make_opener_fill(87, "BTCUSDT", order_side::buy, 1.0, 100.0));

    fake->next_handles = {};
    fake->next_handles.sl_exchange_id = "sl-sequence-two";
    m.register_pending(make_long_intent("mr", "BTCUSDT", 88, 95.0, 110.0));
    m.on_fill(make_opener_fill(88, "BTCUSDT", order_side::buy, 1.0, 100.0));

    constexpr std::uint64_t source_sequence = 8'701;
    const auto first = m.resolve_native_bracket_update(native_economic_update(
        native_bracket_lifecycle::full_fill, "sl-sequence-one", "BTCUSDT", "",
        "trade-87", 1.0, 1.0, 99.0, 1'700'000'000'187, source_sequence));
    ASSERT_EQ(first.kind, native_bracket_resolution_kind::economic_fill);
    const auto duplicate_source = m.resolve_native_bracket_update(
        native_economic_update(native_bracket_lifecycle::full_fill,
                               "sl-sequence-two", "BTCUSDT", "", "trade-88",
                               1.0, 1.0, 99.0, 1'700'000'000'188,
                               source_sequence));
    EXPECT_EQ(duplicate_source.kind, native_bracket_resolution_kind::fatal);
    EXPECT_TRUE(m.native_bracket_requires_reconciliation());
}

TEST(ExitManagerNativeBracket,
     GroupAllDoneIsMonotonicAndNeedsChildOutcomeProof)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-group-order";
    fake->next_handles.tp_exchange_id = "tp-group-order";
    fake->next_handles.oco_list_id = "oco-group-order";
    m.set_bracket_adapter(fake);
    m.register_pending(make_long_intent("mr", "BTCUSDT", 84, 95.0, 110.0));
    m.on_fill(make_opener_fill(84, "BTCUSDT", order_side::buy, 1.0, 100.0));

    native_bracket_group_update all_done;
    all_done.status = native_bracket_group_status::completed;
    all_done.group_id = "oco-group-order";
    all_done.symbol = "BTCUSDT";
    all_done.event_time_ms = 1'700'000'000'184;
    EXPECT_EQ(m.resolve_native_bracket_group_update(all_done),
              native_bracket_group_resolution::lifecycle);
    EXPECT_TRUE(m.has_unresolved_native_bracket_lifecycle());

    auto stale_active = all_done;
    stale_active.status = native_bracket_group_status::active;
    stale_active.event_time_ms = 1'700'000'000'185;
    EXPECT_EQ(m.resolve_native_bracket_group_update(stale_active),
              native_bracket_group_resolution::fatal);
    EXPECT_TRUE(m.native_bracket_requires_reconciliation());
}

TEST(ExitManagerNativeBracket,
     IndependentSiblingCancelIsPostCommitAndAmbiguityRetainsProof)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-independent";
    fake->next_handles.tp_exchange_id = "tp-independent";
    m.set_bracket_adapter(fake);
    m.register_pending(make_long_intent("mr", "BTCUSDT", 85, 95.0, 110.0));
    m.on_fill(make_opener_fill(85, "BTCUSDT", order_side::buy, 1.0, 100.0));

    const auto fill = native_economic_update(
        native_bracket_lifecycle::full_fill, "sl-independent", "BTCUSDT", "",
        "trade-85", 1.0, 1.0, 99.0, 1'700'000'000'186, 8'501);
    const auto winner = m.resolve_native_bracket_update(fill);
    ASSERT_EQ(winner.kind, native_bracket_resolution_kind::economic_fill);
    ASSERT_TRUE(m.commit_native_bracket_economic(winner.reservation));
    EXPECT_EQ(m.request_native_bracket_sibling_cancel(winner.reservation),
              native_bracket_sibling_cancel_result::requested);
    EXPECT_EQ(fake->cancel_calls, 1);
    EXPECT_EQ(m.resolve_native_bracket_update(native_terminal_update(
                  native_bracket_lifecycle::canceled, "tp-independent", "BTCUSDT",
                  "", 0.0)).kind,
              native_bracket_resolution_kind::lifecycle);
    EXPECT_FALSE(m.has_unresolved_native_bracket_lifecycle());
    // The active handles can retire, but the reverse identity never rolls
    // back: a later replay/conflict remains attributable.
    EXPECT_EQ(m.opener_for_exchange_order("sl-independent"), 85u);

    ExitManager ambiguous;
    auto ambiguous_fake = std::make_shared<FakeAdapter>();
    ambiguous_fake->next_handles.sl_exchange_id = "sl-ambiguous";
    ambiguous_fake->next_handles.tp_exchange_id = "tp-ambiguous";
    ambiguous_fake->throw_cancel = true;
    ambiguous.set_bracket_adapter(ambiguous_fake);
    ambiguous.register_pending(make_long_intent("mr", "BTCUSDT", 86, 95.0, 110.0));
    ambiguous.on_fill(make_opener_fill(86, "BTCUSDT", order_side::buy, 1.0, 100.0));
    const auto ambiguous_fill = native_economic_update(
        native_bracket_lifecycle::full_fill, "sl-ambiguous", "BTCUSDT", "",
        "trade-86", 1.0, 1.0, 99.0, 1'700'000'000'187, 8'601);
    const auto ambiguous_winner = ambiguous.resolve_native_bracket_update(
        ambiguous_fill);
    ASSERT_EQ(ambiguous_winner.kind, native_bracket_resolution_kind::economic_fill);
    ASSERT_TRUE(ambiguous.commit_native_bracket_economic(
        ambiguous_winner.reservation));
    EXPECT_EQ(ambiguous.request_native_bracket_sibling_cancel(
                  ambiguous_winner.reservation),
              native_bracket_sibling_cancel_result::fatal);
    EXPECT_TRUE(ambiguous.native_bracket_requires_reconciliation());
    // The expected sibling terminal remains valid proof despite the ambiguous
    // REST response; it must not be reset to active.
    EXPECT_EQ(ambiguous.resolve_native_bracket_update(native_terminal_update(
                  native_bracket_lifecycle::canceled, "tp-ambiguous", "BTCUSDT",
                  "", 0.0)).kind,
              native_bracket_resolution_kind::lifecycle);
}

TEST(ExitManagerNativeBracket,
     IndependentCancelThenEconomicSiblingFillIsAttributedAndReconciled)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-independent-race";
    fake->next_handles.tp_exchange_id = "tp-independent-race";
    m.set_bracket_adapter(fake);
    m.register_pending(make_long_intent("mr", "BTCUSDT", 90, 95.0, 110.0));
    m.on_fill(make_opener_fill(90, "BTCUSDT", order_side::buy, 1.0, 100.0));

    m.cancel(90);
    ASSERT_EQ(m.resolve_native_bracket_update(native_terminal_update(
                  native_bracket_lifecycle::canceled, "tp-independent-race",
                  "BTCUSDT", "", 0.0)).kind,
              native_bracket_resolution_kind::lifecycle);
    const auto raced = m.resolve_native_bracket_update(native_economic_update(
        native_bracket_lifecycle::full_fill, "sl-independent-race", "BTCUSDT",
        "", "trade-90", 1.0, 1.0, 99.0, 1'700'000'000'190, 9'001));
    EXPECT_EQ(raced.kind,
              native_bracket_resolution_kind::economic_fill_requires_reconciliation);
    ASSERT_TRUE(m.commit_native_bracket_economic(raced.reservation));
    EXPECT_TRUE(m.native_bracket_requires_reconciliation());
}

TEST(ExitManagerNativeBracket, ExpectedOcoSiblingTerminalCompletesButStaysReplayAddressable)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-oco";
    fake->next_handles.tp_exchange_id = "tp-oco";
    fake->next_handles.oco_list_id = "oco-72";
    m.set_bracket_adapter(fake);
    m.register_pending(make_long_intent("mr", "BTCUSDT", 72, 95.0, 110.0));
    m.on_fill(make_opener_fill(72, "BTCUSDT", order_side::buy, 1.0, 100.0));

    const auto winner = m.resolve_native_bracket_update(native_economic_update(
        native_bracket_lifecycle::full_fill, "sl-oco", "BTCUSDT", "oco-72",
        "trade-72", 1.0, 1.0));
    ASSERT_EQ(winner.kind, native_bracket_resolution_kind::economic_fill);
    ASSERT_TRUE(m.commit_native_bracket_economic(winner.reservation));
    // A real list/OCO owns sibling cancellation atomically. ExitManager waits
    // for child confirmation + ALL_DONE rather than issuing a second REST
    // cancel that would weaken the venue's proof model.
    EXPECT_EQ(m.request_native_bracket_sibling_cancel(winner.reservation),
              native_bracket_sibling_cancel_result::not_required);
    EXPECT_EQ(fake->cancel_calls, 0);

    native_bracket_group_update completed;
    completed.status = native_bracket_group_status::completed;
    completed.group_id = "oco-72";
    completed.symbol = "BTCUSDT";
    completed.event_time_ms = 1'700'000'000'050;
    EXPECT_EQ(m.resolve_native_bracket_group_update(completed),
              native_bracket_group_resolution::lifecycle);
    EXPECT_TRUE(m.has_unresolved_native_bracket_lifecycle());

    EXPECT_EQ(m.resolve_native_bracket_update(native_terminal_update(
                  native_bracket_lifecycle::canceled, "tp-oco", "BTCUSDT",
                  "oco-72", 0.0)).kind,
              native_bracket_resolution_kind::lifecycle);
    EXPECT_FALSE(m.native_bracket_requires_reconciliation());
    EXPECT_FALSE(m.has_unresolved_native_bracket_lifecycle());
    EXPECT_FALSE(m.native_bracket_blocks_symbol_admission("bTcUsDt"));
    // Identities are intentionally retained for exact replay/conflict proof.
    EXPECT_EQ(m.opener_for_exchange_order("tp-oco"), 72u);
}

TEST(ExitManagerNativeBracket, SiblingEconomicRaceIsAttributedThenRequiresReconciliation)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-race";
    fake->next_handles.tp_exchange_id = "tp-race";
    fake->next_handles.oco_list_id = "oco-race";
    m.set_bracket_adapter(fake);
    m.register_pending(make_long_intent("mr", "BTCUSDT", 73, 95.0, 110.0));
    m.on_fill(make_opener_fill(73, "BTCUSDT", order_side::buy, 1.0, 100.0));

    const auto winner = m.resolve_native_bracket_update(native_economic_update(
        native_bracket_lifecycle::full_fill, "sl-race", "BTCUSDT", "oco-race",
        "trade-73-sl", 1.0, 1.0));
    ASSERT_EQ(winner.kind, native_bracket_resolution_kind::economic_fill);
    ASSERT_TRUE(m.commit_native_bracket_economic(winner.reservation));
    const auto raced = m.resolve_native_bracket_update(native_economic_update(
        native_bracket_lifecycle::full_fill, "tp-race", "BTCUSDT", "oco-race",
        "trade-73-tp", 1.0, 1.0, 111.0, 1'700'000'000'010));
    EXPECT_EQ(raced.kind,
              native_bracket_resolution_kind::economic_fill_requires_reconciliation);
    EXPECT_EQ(raced.opener_order_id, 73u);
    EXPECT_EQ(raced.strategy_name, "mr");
    ASSERT_TRUE(m.commit_native_bracket_economic(raced.reservation));
    EXPECT_TRUE(m.native_bracket_requires_reconciliation());
}

TEST(ExitManagerNativeBracket, UnrequestedProtectionLossAndMissedSiblingDeadlineFailClosed)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-deadline";
    fake->next_handles.tp_exchange_id = "tp-deadline";
    fake->next_handles.oco_list_id = "oco-deadline";
    m.set_bracket_adapter(fake);
    m.register_pending(make_long_intent("mr", "BTCUSDT", 74, 95.0, 110.0));
    m.on_fill(make_opener_fill(74, "BTCUSDT", order_side::buy, 1.0, 100.0));

    // A live protective leg cannot disappear without a local cancel request
    // or a recorded OCO winner.
    EXPECT_EQ(m.resolve_native_bracket_update(native_terminal_update(
                  native_bracket_lifecycle::canceled, "sl-deadline", "BTCUSDT",
                  "oco-deadline", 0.0)).kind,
              native_bracket_resolution_kind::fatal);
    EXPECT_TRUE(m.native_bracket_requires_reconciliation());

    ExitManager deadline_manager;
    auto deadline_fake = std::make_shared<FakeAdapter>();
    deadline_fake->next_handles.sl_exchange_id = "sl-await";
    deadline_fake->next_handles.tp_exchange_id = "tp-await";
    deadline_fake->next_handles.oco_list_id = "oco-await";
    deadline_manager.set_bracket_adapter(deadline_fake);
    deadline_manager.register_pending(
        make_long_intent("mr", "BTCUSDT", 75, 95.0, 110.0));
    deadline_manager.on_fill(
        make_opener_fill(75, "BTCUSDT", order_side::buy, 1.0, 100.0));
    const auto winner = deadline_manager.resolve_native_bracket_update(
        native_economic_update(native_bracket_lifecycle::full_fill, "sl-await",
                               "BTCUSDT", "oco-await", "trade-75", 1.0, 1.0));
    ASSERT_EQ(winner.kind, native_bracket_resolution_kind::economic_fill);
    ASSERT_TRUE(deadline_manager.commit_native_bracket_economic(winner.reservation));

    const auto after_winner = std::chrono::steady_clock::now();
    EXPECT_TRUE(deadline_manager.check_native_bracket_lifecycle_deadline(
        after_winner + std::chrono::seconds{29}));
    EXPECT_FALSE(deadline_manager.check_native_bracket_lifecycle_deadline(
        after_winner + std::chrono::seconds{31}));
    EXPECT_TRUE(deadline_manager.native_bracket_requires_reconciliation());
}

TEST(ExitManagerNativeBracket, ManualReleaseAcceptsBothPrivateCancelsThenGroupCompletion)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-manual";
    fake->next_handles.tp_exchange_id = "tp-manual";
    fake->next_handles.oco_list_id = "oco-manual";
    m.set_bracket_adapter(fake);
    m.register_pending(make_long_intent("mr", "BTCUSDT", 76, 95.0, 110.0));
    m.on_fill(make_opener_fill(76, "BTCUSDT", order_side::buy, 1.0, 100.0));

    m.cancel(76);
    ASSERT_EQ(fake->cancel_calls, 1);
    EXPECT_EQ(m.resolve_native_bracket_update(native_terminal_update(
                  native_bracket_lifecycle::canceled, "sl-manual", "BTCUSDT",
                  "oco-manual", 0.0)).kind,
              native_bracket_resolution_kind::lifecycle);
    EXPECT_EQ(m.resolve_native_bracket_update(native_terminal_update(
                  native_bracket_lifecycle::canceled, "tp-manual", "BTCUSDT",
                  "oco-manual", 0.0, 1'700'000'000'101)).kind,
              native_bracket_resolution_kind::lifecycle);
    // Child cancel reports alone do not retire an OCO/list. The retained
    // group must still observe the venue's ALL_DONE lifecycle.
    EXPECT_TRUE(m.has_unresolved_native_bracket_lifecycle());

    native_bracket_group_update all_done;
    all_done.status = native_bracket_group_status::completed;
    all_done.group_id = "oco-manual";
    all_done.symbol = "BTCUSDT";
    all_done.event_time_ms = 1'700'000'000'102;
    EXPECT_EQ(m.resolve_native_bracket_group_update(all_done),
              native_bracket_group_resolution::lifecycle);
    EXPECT_FALSE(m.has_unresolved_native_bracket_lifecycle());
    EXPECT_FALSE(m.native_bracket_requires_reconciliation());

    // The retained terminal identity still accepts late venue economics for
    // accounting, but it refuses to call that a clean completed OCO.
    const auto late_fill = m.resolve_native_bracket_update(native_economic_update(
        native_bracket_lifecycle::full_fill, "tp-manual", "BTCUSDT",
        "oco-manual", "trade-76-late", 1.0, 1.0, 111.0,
        1'700'000'000'103));
    EXPECT_EQ(late_fill.kind,
              native_bracket_resolution_kind::economic_fill_requires_reconciliation);
    EXPECT_EQ(late_fill.opener_order_id, 76u);
    // Reserve itself closes the former clean-ALL_DONE admission window, even
    // before engine accounting has had a chance to commit the late venue fact.
    EXPECT_TRUE(m.native_bracket_blocks_symbol_admission("btcusdt"));
    EXPECT_FALSE(m.native_bracket_requires_reconciliation());
    ASSERT_TRUE(m.commit_native_bracket_economic(late_fill.reservation));
    EXPECT_TRUE(m.native_bracket_requires_reconciliation());
}

TEST(ExitManagerNativeBracket,
     ManualTerminalChildrenWithoutAllDoneExpireFailClosed)
{
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-manual-timeout";
    fake->next_handles.tp_exchange_id = "tp-manual-timeout";
    fake->next_handles.oco_list_id = "oco-manual-timeout";
    m.set_bracket_adapter(fake);
    m.register_pending(make_long_intent("mr", "BTCUSDT", 89, 95.0, 110.0));
    m.on_fill(make_opener_fill(89, "BTCUSDT", order_side::buy, 1.0, 100.0));

    m.cancel(89);
    ASSERT_EQ(m.resolve_native_bracket_update(native_terminal_update(
                  native_bracket_lifecycle::canceled, "sl-manual-timeout",
                  "BTCUSDT", "oco-manual-timeout", 0.0)).kind,
              native_bracket_resolution_kind::lifecycle);
    ASSERT_EQ(m.resolve_native_bracket_update(native_terminal_update(
                  native_bracket_lifecycle::canceled, "tp-manual-timeout",
                  "BTCUSDT", "oco-manual-timeout", 0.0,
                  1'700'000'000'201)).kind,
              native_bracket_resolution_kind::lifecycle);

    const auto after_children = std::chrono::steady_clock::now();
    EXPECT_TRUE(m.check_native_bracket_lifecycle_deadline(
        after_children + std::chrono::seconds{29}));
    EXPECT_FALSE(m.check_native_bracket_lifecycle_deadline(
        after_children + std::chrono::seconds{31}));
    EXPECT_TRUE(m.native_bracket_requires_reconciliation());
}

TEST(ExitManagerNativeBracket, BoundedLongAndRecoveredEmptyStrategyAttributionAreExplicit)
{
    const std::string long_strategy(80, 's');
    ExitManager m;
    auto fake = std::make_shared<FakeAdapter>();
    fake->next_handles.sl_exchange_id = "sl-long-strategy";
    m.set_bracket_adapter(fake);
    m.register_pending(make_long_intent(long_strategy, "BTCUSDT", 77, 95.0, 110.0));
    m.on_fill(make_opener_fill(77, "BTCUSDT", order_side::buy, 1.0, 100.0));
    const auto long_result = m.resolve_native_bracket_update(native_economic_update(
        native_bracket_lifecycle::full_fill, "sl-long-strategy", "BTCUSDT", "",
        "trade-77", 1.0, 1.0));
    EXPECT_EQ(long_result.kind, native_bracket_resolution_kind::economic_fill);
    EXPECT_EQ(long_result.strategy_name, long_strategy);
    ASSERT_TRUE(m.commit_native_bracket_economic(long_result.reservation));

    ExitManager recovered;
    IBracketAdapter::recovered_bracket rb;
    rb.opener_order_id = 78;
    rb.symbol = "BTCUSDT";
    rb.close_side = order_side::sell;
    rb.qty = 1.0;
    rb.entry_price = 100.0;
    rb.handles.sl_exchange_id = "sl-recovered-empty";
    ASSERT_NO_THROW(recovered.rehydrate(rb));
    const auto recovered_result = recovered.resolve_native_bracket_update(
        native_economic_update(native_bracket_lifecycle::full_fill,
                                "sl-recovered-empty", "BTCUSDT", "",
                                "trade-78", 1.0, 1.0));
    EXPECT_EQ(recovered_result.kind, native_bracket_resolution_kind::economic_fill);
    EXPECT_EQ(recovered_result.opener_order_id, 78u);
    EXPECT_TRUE(recovered_result.strategy_name.empty());
    ASSERT_TRUE(recovered.commit_native_bracket_economic(
        recovered_result.reservation));
}

TEST(ExitManagerAdapter, NoAdapterMeansNoCallsAndExchangeMapAlwaysZero)
{
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, 95.0, 110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    EXPECT_FALSE(m.has_bracket_adapter());
    EXPECT_EQ(m.opener_for_exchange_order("anything"), 0u);

    auto r = m.on_price("X", 94.0, t0);
    ASSERT_FALSE(r.empty());  // engine-side eval still works
}

TEST(ExitManager, OpenersFor_TracksPendingAndArmed)
{
    ExitManager m;
    EXPECT_EQ(m.openers_for("s", "X"), 0u);

    m.register_pending(make_long_intent("s", "X", 1, 95.0, 110.0));
    EXPECT_EQ(m.openers_for("s", "X"), 1u);

    // Second concurrent opener (multi-lot pattern).
    m.register_pending(make_long_intent("s", "X", 2, 95.0, 110.0));
    EXPECT_EQ(m.openers_for("s", "X"), 2u);

    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));
    EXPECT_EQ(m.openers_for("s", "X"), 2u);  // promoted, still live

    // Trigger - opener 1's intent fires and is untracked.
    auto r = m.on_price("X", 95.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(m.openers_for("s", "X"), 1u);
}
