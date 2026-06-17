// Pins the shadow-mode "what would have filled on the tape" matcher.
// The adapter replaces the old HybridExecutor-in-paper-mode wired into
// shadow, so tests here guard against a regression back toward sim-vs-sim.

#include <gtest/gtest.h>
#include "execution/trade_tape_shadow_adapter.h"
#include "execution/latency_model.h"

#include <chrono>
#include <memory>
#include <string>

using tp = std::chrono::system_clock::time_point;
using ms = std::chrono::milliseconds;

namespace {

order_event make_limit(uint64_t id, order_side side, double price, double qty,
                       tp submit_ts, const std::string& sym = "X")
{
    order_event o(submit_ts, sym, order_type::limit, side, qty, price,
                  time_in_force::gtc);
    o.set_order_id(id);
    return o;
}

order_event make_market(uint64_t id, order_side side, double qty, tp submit_ts,
                        const std::string& sym = "X")
{
    order_event o(submit_ts, sym, order_type::market, side, qty, 0.0,
                  time_in_force::ioc);
    o.set_order_id(id);
    return o;
}

}

TEST(TradeTapeShadowAdapter, BuyLimit_CrossingTradeFillsAtTradePrice)
{
    TradeTapeShadowAdapter a;
    a.submit_order(make_limit(1, order_side::buy, 100.0, 1.0, tp{}));
    a.on_trade("X", 99.5, 2.0, tp{ms(100)});

    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].get_filled_quantity(), 1.0);
    EXPECT_DOUBLE_EQ(fills[0].get_fill_price(), 99.5);
    EXPECT_EQ(fills[0].get_source(), fill_source::exchange);
    EXPECT_EQ(a.open_order_count(), 0u);
}

TEST(TradeTapeShadowAdapter, BuyLimit_TradeAbovePriceDoesNotFill)
{
    TradeTapeShadowAdapter a;
    a.submit_order(make_limit(1, order_side::buy, 100.0, 1.0, tp{}));
    a.on_trade("X", 100.1, 2.0, tp{ms(100)});

    std::vector<fill_event> fills;
    EXPECT_FALSE(a.poll_fills(fills));
    EXPECT_EQ(a.open_order_count(), 1u);
}

TEST(TradeTapeShadowAdapter, SellLimit_TradeAtOrAbovePriceFills)
{
    TradeTapeShadowAdapter a;
    a.submit_order(make_limit(2, order_side::sell, 100.0, 1.0, tp{}));

    // Below limit: no fill.
    a.on_trade("X", 99.0, 2.0, tp{ms(100)});
    std::vector<fill_event> fills;
    EXPECT_FALSE(a.poll_fills(fills));
    fills.clear();

    // At-or-above: fills.
    a.on_trade("X", 101.0, 2.0, tp{ms(200)});
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].get_fill_price(), 101.0);
}

TEST(TradeTapeShadowAdapter, Market_FillsAtNextTradeRegardlessOfSide)
{
    TradeTapeShadowAdapter a;
    a.submit_order(make_market(3, order_side::buy, 1.0, tp{}));
    a.on_trade("X", 42.0, 5.0, tp{ms(50)});

    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].get_fill_price(), 42.0);
}

TEST(TradeTapeShadowAdapter, PartialFill_RemainingQtyStaysOpen)
{
    TradeTapeShadowAdapter a;
    a.submit_order(make_limit(4, order_side::buy, 100.0, 10.0, tp{}));
    a.on_trade("X", 99.0, 3.0, tp{ms(100)});

    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].get_filled_quantity(), 3.0);
    EXPECT_DOUBLE_EQ(fills[0].get_remaining_qty(), 7.0);
    EXPECT_TRUE(fills[0].is_partial());
    EXPECT_EQ(a.open_order_count(), 1u);

    // Second crossing trade fills the remainder. poll_fills appends to
    // `out`, so clear before re-polling to assert only the new fill.
    fills.clear();
    a.on_trade("X", 98.5, 7.0, tp{ms(200)});
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].get_filled_quantity(), 7.0);
    EXPECT_DOUBLE_EQ(fills[0].get_remaining_qty(), 0.0);
    EXPECT_EQ(a.open_order_count(), 0u);
}

TEST(TradeTapeShadowAdapter, PreSubmitTradeIgnored)
{
    // Order submitted at t=200; trade at t=100 must not back-fill it.
    TradeTapeShadowAdapter a;
    a.submit_order(make_limit(5, order_side::buy, 100.0, 1.0, tp{ms(200)}));
    a.on_trade("X", 99.0, 1.0, tp{ms(100)});

    std::vector<fill_event> fills;
    EXPECT_FALSE(a.poll_fills(fills));
    EXPECT_EQ(a.open_order_count(), 1u);
    fills.clear();

    // A later crossing trade does fill it.
    a.on_trade("X", 99.0, 1.0, tp{ms(300)});
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
}

TEST(TradeTapeShadowAdapter, Cancel_RemovesOrderBeforeMatch)
{
    TradeTapeShadowAdapter a;
    a.submit_order(make_limit(6, order_side::buy, 100.0, 1.0, tp{}));
    EXPECT_TRUE(a.cancel_order(6));
    EXPECT_EQ(a.open_order_count(), 0u);

    a.on_trade("X", 99.0, 1.0, tp{ms(100)});
    std::vector<fill_event> fills;
    EXPECT_FALSE(a.poll_fills(fills));
}

TEST(TradeTapeShadowAdapter, Modify_UpdatesPriceAndQty)
{
    TradeTapeShadowAdapter a;
    a.submit_order(make_limit(7, order_side::buy, 100.0, 1.0, tp{}));
    EXPECT_TRUE(a.modify_order(7, 105.0, 2.0));

    // New price is above original - trade at 103 would now fill it.
    a.on_trade("X", 103.0, 10.0, tp{ms(100)});
    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].get_filled_quantity(), 2.0);
}

TEST(TradeTapeShadowAdapter, ForeignSymbolIgnored)
{
    TradeTapeShadowAdapter a;
    a.submit_order(make_limit(8, order_side::buy, 100.0, 1.0, tp{}, "X"));
    a.on_trade("Y", 99.0, 5.0, tp{ms(100)});

    std::vector<fill_event> fills;
    EXPECT_FALSE(a.poll_fills(fills));
    EXPECT_EQ(a.open_order_count(), 1u);
}

TEST(TradeTapeShadowAdapter, TapeQtyCapsTotalFilledAcrossOrders)
{
    // Two 10-lot buys at 100; one trade of 15 at 99. Trade qty caps how
    // much we can synthesize - the second order only gets 5.
    TradeTapeShadowAdapter a;
    a.submit_order(make_limit(9,  order_side::buy, 100.0, 10.0, tp{}));
    a.submit_order(make_limit(10, order_side::buy, 100.0, 10.0, tp{}));
    a.on_trade("X", 99.0, 15.0, tp{ms(100)});

    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 2u);
    double total = fills[0].get_filled_quantity() + fills[1].get_filled_quantity();
    EXPECT_DOUBLE_EQ(total, 15.0);
    EXPECT_EQ(a.open_order_count(), 1u);  // remaining 5 lots on order 10 stay live
}

TEST(TradeTapeShadowAdapter, SubmitTs_UsesEarliestEligibleTs)
{
    // When the engine defers an order via its own latency_model, it bumps
    // earliest_eligible_ts past the creation timestamp. The shadow adapter
    // must gate matches against eligible_ts, not creation_ts - otherwise
    // trades that printed during the engine-side latency window would
    // back-fill orders the real exchange hadn't seen yet.
    TradeTapeShadowAdapter a;
    order_event o = make_limit(1, order_side::buy, 100.0, 1.0, tp{ms(0)});
    o.set_earliest_eligible_ts(tp{ms(500)});
    a.submit_order(o);

    // Trade during the engine-latency window (t=100): must NOT match,
    // because eligible_ts=500.
    a.on_trade("X", 99.0, 1.0, tp{ms(100)});
    std::vector<fill_event> fills;
    EXPECT_FALSE(a.poll_fills(fills));
    EXPECT_EQ(a.open_order_count(), 1u);

    // Trade at eligible_ts: matches.
    a.on_trade("X", 99.0, 1.0, tp{ms(500)});
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
}

TEST(TradeTapeShadowAdapter, WireLatency_TradesDuringWindowMiss)
{
    // An extra shadow-side wire latency pushes arrival past the engine's
    // eligible_ts. A trade printed between eligible_ts and
    // eligible_ts + wire_latency must not fill the shadow order - this is
    // the whole point of the model: sim books the fill, shadow doesn't.
    auto latency = std::make_shared<FixedLatencyModel>(
        latency_duration(1000));  // 1 ms wire latency
    TradeTapeShadowAdapter a(latency);

    order_event o = make_limit(2, order_side::buy, 100.0, 1.0, tp{ms(0)});
    // earliest_eligible_ts defaults to creation ts (ms 0) - shadow
    // arrival becomes ms 0 + 1ms = ms 1.
    a.submit_order(o);

    // Within the wire-latency window: ignored.
    a.on_trade("X", 99.0, 1.0, tp{std::chrono::microseconds(500)});
    std::vector<fill_event> fills;
    EXPECT_FALSE(a.poll_fills(fills));
    EXPECT_EQ(a.open_order_count(), 1u);

    // At exactly the arrival instant: eligible.
    a.on_trade("X", 99.0, 1.0, tp{ms(1)});
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_source(), fill_source::exchange);
}

TEST(TradeTapeShadowAdapter, WireLatency_StacksOnEngineLatency)
{
    // Both layers compose: the engine bumps eligible_ts to 500us, then
    // the shadow adapter adds another 500us of wire latency. A trade at
    // 800us is past eligibility but still in the wire window - must miss.
    // A trade at 1100us is past both gates and fills.
    auto latency = std::make_shared<FixedLatencyModel>(
        latency_duration(500));
    TradeTapeShadowAdapter a(latency);

    order_event o = make_limit(3, order_side::sell, 100.0, 1.0, tp{ms(0)});
    o.set_earliest_eligible_ts(tp{std::chrono::microseconds(500)});
    a.submit_order(o);

    a.on_trade("X", 101.0, 1.0, tp{std::chrono::microseconds(800)});
    std::vector<fill_event> fills;
    EXPECT_FALSE(a.poll_fills(fills));
    EXPECT_EQ(a.open_order_count(), 1u);

    a.on_trade("X", 101.0, 1.0, tp{std::chrono::microseconds(1100)});
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
}

TEST(TradeTapeShadowAdapter, ZeroLatency_SameAsNoLatencyModel)
{
    // Regression guard: passing a ZeroLatencyModel is observably equivalent
    // to not passing one - simplifies wiring for call sites that always
    // want to construct with a model.
    TradeTapeShadowAdapter a(std::make_shared<ZeroLatencyModel>());
    a.submit_order(make_limit(4, order_side::buy, 100.0, 1.0, tp{}));
    a.on_trade("X", 99.5, 1.0, tp{ms(0)});

    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].get_fill_price(), 99.5);
}
