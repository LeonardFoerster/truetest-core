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
    EXPECT_FALSE(r.has_value());
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
    EXPECT_FALSE(m.on_price("X", 96.0, t0).has_value());

    // At SL — fire, market sell, correct qty.
    auto r = m.on_price("X", 95.0, t0);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->get_symbol(), "X");
    EXPECT_EQ(r->get_side(), order_side::sell);
    EXPECT_EQ(r->get_order_type(), order_type::market);
    EXPECT_DOUBLE_EQ(r->get_quantity(), 2.0);  // from opener fill qty, not intent qty
    // Second tick at the same price shouldn't re-fire — the intent is
    // gone after trigger.
    EXPECT_FALSE(m.on_price("X", 95.0, t0).has_value());
    EXPECT_EQ(m.armed_count(), 0u);
}

TEST(ExitManager, TakeProfitFiresAtOrAboveThreshold)
{
    ExitManager m;
    m.register_pending(make_long_intent("s", "X", 1, /*sl=*/90.0, /*tp=*/110.0));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    EXPECT_FALSE(m.on_price("X", 109.99, t0).has_value());
    auto r = m.on_price("X", 110.0, t0);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->get_side(), order_side::sell);
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
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(r->get_price(), 91.0);
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

    EXPECT_FALSE(m.on_price("X", 103.0, t0).has_value());  // trail raises SL to 101.97
    EXPECT_FALSE(m.on_price("X", 105.0, t0).has_value());  // trail raises SL to 103.95
    EXPECT_FALSE(m.on_price("X", 104.0, t0).has_value());  // above trailed SL
    auto r = m.on_price("X", 103.90, t0);
    ASSERT_TRUE(r.has_value());
}

TEST(ExitManager, TimeStopFiresAfterDeadline)
{
    ExitManager m;
    exit_intent ei = make_long_intent("s", "X", 1, std::nullopt, std::nullopt);
    ei.deadline = t0 + std::chrono::seconds(5);
    m.register_pending(std::move(ei));
    m.on_fill(make_opener_fill(1, "X", order_side::buy, 1.0, 100.0));

    EXPECT_FALSE(m.on_price("X", 100.0, t0 + std::chrono::seconds(4)).has_value());
    auto r = m.on_price("X", 100.0, t0 + std::chrono::seconds(5));
    ASSERT_TRUE(r.has_value());
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
    EXPECT_FALSE(m.on_price("Y", 95.0, t0).has_value());
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
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(r->get_quantity(), 0.6);
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
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(m.armed_count(), 1u);

    // Tick at 89 triggers b.
    auto r2 = m.on_price("X", 89.0, t0);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(m.armed_count(), 0u);
}
