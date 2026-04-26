// Pins the engine-side SL/TP/trailing enforcement module.
//
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

} // namespace

TEST(ExitManager, PendingOnlyFiresAfterOpenerFill)
{
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 42, /*sl=*/90.0, /*tp=*/110.0));

    // Before the opener fills, no price event can trigger — the intent
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

    // Above SL — no fire.
    EXPECT_TRUE(m.on_price("X", 96.0, t0).empty());

    // At SL — fire, market sell, correct qty.
    auto r = m.on_price("X", 95.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r[0].get_symbol(), "X");
    EXPECT_EQ(r[0].get_side(), order_side::sell);
    EXPECT_EQ(r[0].get_order_type(), order_type::market);
    EXPECT_DOUBLE_EQ(r[0].get_quantity(), 2.0);  // from opener fill qty, not intent qty
    // Second tick at the same price shouldn't re-fire — the intent is
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
    // 1% trailing: entry 100, best=105 → trailed SL = 103.95.
    // Then price drops to 103.9 → SL fires at 103.9, not original 95.
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

    // 93: between a.sl and b.sl — only a should fire.
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

    // Touch 110 — TP1 fires, closing 5 units. TP2 and SL remain armed.
    auto r1 = m.on_price("X", 110.0, t0);
    ASSERT_FALSE(r1.empty());
    EXPECT_NEAR(r1[0].get_quantity(), 5.0, 1e-9);
    EXPECT_EQ(m.armed_count(), 2u);

    // Touch 120 — TP2 fires, closing 3 units. SL remains.
    auto r2 = m.on_price("X", 120.0, t0);
    ASSERT_FALSE(r2.empty());
    EXPECT_NEAR(r2[0].get_quantity(), 3.0, 1e-9);
    EXPECT_EQ(m.armed_count(), 1u);

    // Crash to 89 — SL fires for the full 10 units (its own fraction=1.0
    // binds to opener_qty, independent of earlier fires).
    auto r3 = m.on_price("X", 89.0, t0);
    ASSERT_FALSE(r3.empty());
    EXPECT_NEAR(r3[0].get_quantity(), 10.0, 1e-9);
    EXPECT_EQ(m.armed_count(), 0u);
}

TEST(ExitManager, PartialExit_DefaultFractionIsFullClose)
{
    // No qty_fraction set → default 1.0 → close_qty = opener_qty.
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
    bad.qty_fraction = -0.5;                   // nonsensical → treated as 1.0
    m.register_pending(std::move(bad));

    auto too_big = make_long_intent("s", "Y", 2, 90.0, 110.0, 10.0);
    too_big.qty_fraction = 1.5;                // > 1.0 → clamped to 1.0
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
    // Bar wicks below SL but closes above — on_price(close) would miss it,
    // on_bar must catch it.
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, /*sl=*/95.0, /*tp=*/110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    auto r = m.on_bar("X", /*low=*/94.0, /*high=*/101.0, /*close=*/100.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r[0].get_side(), order_side::sell);
    EXPECT_DOUBLE_EQ(r[0].get_price(), 94.0);  // fired at observed extreme
    EXPECT_EQ(m.armed_count(), 0u);
}

TEST(ExitManager, OnBar_LongTpFiresOnHighEvenWhenCloseDoesntReach)
{
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, 95.0, 110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    auto r = m.on_bar("X", 99.0, 112.0, 105.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_DOUBLE_EQ(r[0].get_price(), 112.0);
}

TEST(ExitManager, OnBar_BothTouchedSlWinsForLong)
{
    // Conservative: when both extremes cross in one bar, SL fires (worst).
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, 95.0, 110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    auto r = m.on_bar("X", 94.0, 111.0, 100.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_DOUBLE_EQ(r[0].get_price(), 94.0);  // SL not TP
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

    auto r = m.on_bar("X", /*low=*/96.0, /*high=*/106.0, /*close=*/100.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r[0].get_side(), order_side::buy);
    EXPECT_DOUBLE_EQ(r[0].get_price(), 106.0);
}

TEST(ExitManager, OnBar_NoTriggerLeavesIntentArmed)
{
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, 95.0, 110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    EXPECT_TRUE(m.on_bar("X", 96.0, 109.0, 100.0, t0).empty());
    EXPECT_EQ(m.armed_count(), 1u);
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
    }
};

} // namespace

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
    // place returns empty handles → adapter declined (e.g. only stop_market
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

    // Reverse map cleared so a stale fill on the same exchange id is inert.
    EXPECT_EQ(m.opener_for_exchange_order("sl-9"), 0u);
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

    // Now a price tick crosses SL → ExitManager fires AND cancels
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
    bad.handles = {};                  // empty handles → adapter declined
    m.rehydrate(bad);
    EXPECT_EQ(m.armed_count(), 0u);
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

    // Trigger — opener 1's intent fires and is untracked.
    auto r = m.on_price("X", 95.0, t0);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(m.openers_for("s", "X"), 1u);
}
