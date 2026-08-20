#include <gtest/gtest.h>

#include "exits/default_exit_policy.h"

#include <chrono>

using namespace truetest::exits;

namespace {

auto tp0()
{
    return std::chrono::system_clock::time_point{};
}

order_event make_order(order_side side, double qty, double px,
                       std::uint64_t id = 1, std::uint64_t opener = 0)
{
    order_event o(tp0(), "TEST", order_type::market, side, qty, px);
    o.set_order_id(id);
    if (opener != 0)
        o.set_opener_order_id(opener);
    return o;
}

default_exit_params floor_params(double sl = 0.01, double tp = 0.02)
{
    default_exit_params p;
    p.mode = exit_policy_mode::floor;
    p.sl_pct = sl;
    p.tp_pct = tp;
    return p;
}

} // namespace

TEST(DefaultExitPolicy, StrategyOnlyEmpty)
{
    auto o = make_order(order_side::buy, 10, 100);
    auto out = apply_default_exit_policy(
        default_exit_params{exit_policy_mode::strategy_only, 0.01, 0.02, 0},
        o, 0.0, {});
    EXPECT_TRUE(out.empty());
}

TEST(DefaultExitPolicy, FloorEmptyBuyLong)
{
    auto o = make_order(order_side::buy, 10, 100);
    auto out = apply_default_exit_policy(floor_params(), o, 0.0, {});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].close_side, order_side::sell);
    ASSERT_TRUE(out[0].stop_loss);
    ASSERT_TRUE(out[0].take_profit);
    EXPECT_DOUBLE_EQ(*out[0].stop_loss, 99.0);
    EXPECT_DOUBLE_EQ(*out[0].take_profit, 102.0);
    ASSERT_TRUE(out[0].reference_entry);
    EXPECT_DOUBLE_EQ(*out[0].reference_entry, 100.0);
    EXPECT_DOUBLE_EQ(out[0].qty, 10.0);
}

TEST(DefaultExitPolicy, FloorEmptySellShort)
{
    auto o = make_order(order_side::sell, 5, 200);
    auto out = apply_default_exit_policy(floor_params(0.01, 0.02), o, 0.0, {});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].close_side, order_side::buy);
    ASSERT_TRUE(out[0].stop_loss);
    ASSERT_TRUE(out[0].take_profit);
    EXPECT_DOUBLE_EQ(*out[0].stop_loss, 202.0);
    EXPECT_DOUBLE_EQ(*out[0].take_profit, 196.0);
}

TEST(DefaultExitPolicy, FloorReducingSellNoArm)
{
    auto o = make_order(order_side::sell, 10, 100); // signal close while long
    auto out = apply_default_exit_policy(floor_params(), o, /*net_qty=*/10.0, {});
    EXPECT_TRUE(out.empty());
}

TEST(DefaultExitPolicy, FloorReducingViaOpenerId)
{
    auto o = make_order(order_side::sell, 10, 100, /*id=*/99, /*opener=*/42);
    auto out = apply_default_exit_policy(floor_params(), o, 0.0, {});
    EXPECT_TRUE(out.empty());
}

TEST(DefaultExitPolicy, FloorKeepsStrategyWithSl)
{
    auto o = make_order(order_side::buy, 10, 100);
    exit_intent strat;
    strat.symbol = "TEST";
    strat.close_side = order_side::sell;
    strat.qty = 10;
    strat.stop_loss = 90.0;
    strat.take_profit = 130.0;
    strat.reference_entry = 100.0;

    auto out = apply_default_exit_policy(floor_params(0.01, 0.02), o, 0.0, {strat});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_DOUBLE_EQ(*out[0].stop_loss, 90.0);
    EXPECT_DOUBLE_EQ(*out[0].take_profit, 130.0);
}

TEST(DefaultExitPolicy, FloorPreservesStrategyWithTpOnly)
{
    auto o = make_order(order_side::buy, 10, 100);
    exit_intent strat;
    strat.symbol = "TEST";
    strat.close_side = order_side::sell;
    strat.qty = 10;
    strat.take_profit = 120.0; // TP only

    auto out = apply_default_exit_policy(floor_params(0.01, 0.02), o, 0.0, {strat});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FALSE(out[0].stop_loss.has_value());
    ASSERT_TRUE(out[0].take_profit.has_value());
    EXPECT_DOUBLE_EQ(*out[0].take_profit, 120.0); // strategy TP kept untouched
}

TEST(DefaultExitPolicy, FloorPreservesStrategyWithSlOnly)
{
    auto o = make_order(order_side::buy, 10, 100);
    exit_intent strat;
    strat.symbol = "TEST";
    strat.close_side = order_side::sell;
    strat.qty = 10;
    strat.stop_loss = 90.0; // SL only

    auto out = apply_default_exit_policy(floor_params(0.01, 0.02), o, 0.0, {strat});
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].stop_loss.has_value());
    EXPECT_DOUBLE_EQ(*out[0].stop_loss, 90.0);
    EXPECT_FALSE(out[0].take_profit.has_value()); // no platform TP injected
}

TEST(DefaultExitPolicy, EngineOnlyOnReducingDropsStrategy)
{
    auto o = make_order(order_side::sell, 10, 100);
    exit_intent strat;
    strat.stop_loss = 50.0;
    default_exit_params p = floor_params();
    p.mode = exit_policy_mode::engine_only;
    auto out = apply_default_exit_policy(p, o, /*net=*/10.0, {strat});
    EXPECT_TRUE(out.empty());
}

TEST(DefaultExitPolicy, UnionDoesNotDoubleSl)
{
    auto o = make_order(order_side::buy, 10, 100);
    exit_intent strat;
    strat.symbol = "TEST";
    strat.close_side = order_side::sell;
    strat.qty = 10;
    strat.stop_loss = 95.0;
    strat.take_profit = 120.0;
    default_exit_params p = floor_params(0.01, 0.02);
    p.mode = exit_policy_mode::union_mode;
    auto out = apply_default_exit_policy(p, o, 0.0, {strat});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_DOUBLE_EQ(*out[0].stop_loss, 95.0);
}

TEST(DefaultExitPolicy, InvalidPriceYieldsEmpty)
{
    auto o = make_order(order_side::buy, 10, 0.0);
    auto out = apply_default_exit_policy(floor_params(), o, 0.0, {});
    EXPECT_TRUE(out.empty());
}

TEST(DefaultExitPolicy, EngineOnlyReplacesStrategy)
{
    auto o = make_order(order_side::buy, 10, 100);
    exit_intent strat;
    strat.stop_loss = 50.0;
    strat.take_profit = 200.0;

    default_exit_params p = floor_params(0.01, 0.02);
    p.mode = exit_policy_mode::engine_only;
    auto out = apply_default_exit_policy(p, o, 0.0, {strat});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_DOUBLE_EQ(*out[0].stop_loss, 99.0);
    EXPECT_DOUBLE_EQ(*out[0].take_profit, 102.0);
}

TEST(DefaultExitPolicy, UnionPreservesStrategyIntents)
{
    auto o = make_order(order_side::buy, 10, 100);
    exit_intent strat;
    strat.symbol = "TEST";
    strat.close_side = order_side::sell;
    strat.qty = 10;
    strat.take_profit = 120.0;

    default_exit_params p = floor_params(0.01, 0.02);
    p.mode = exit_policy_mode::union_mode;
    auto out = apply_default_exit_policy(p, o, 0.0, {strat});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FALSE(out[0].stop_loss.has_value());
    ASSERT_TRUE(out[0].take_profit.has_value());
    EXPECT_DOUBLE_EQ(*out[0].take_profit, 120.0);
}

TEST(BacktestDefects, BF04_UnionModeEmptyStrategyKeepsFullPlatformPlan)
{
    auto o = make_order(order_side::sell, 5, 200);
    default_exit_params p;
    p.mode = exit_policy_mode::union_mode;
    p.sl_pct = 0.01;
    p.tp_pct = 0.02;
    p.trail_pct = 0.005;

    auto out = apply_default_exit_policy(p, o, 0.0, {});
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].stop_loss);
    ASSERT_TRUE(out[0].take_profit);
    ASSERT_TRUE(out[0].trailing_pct);
    EXPECT_DOUBLE_EQ(*out[0].stop_loss, 202.0);
    EXPECT_DOUBLE_EQ(*out[0].take_profit, 196.0);
    EXPECT_DOUBLE_EQ(*out[0].trailing_pct, 0.005);
}

TEST(DefaultExitPolicy, MultiStrategyScopedReducing)
{
    auto sell = make_order(order_side::sell, 5, 100);
    // When strategy has 0 net position, sell order is an opener (not reducing)
    EXPECT_FALSE(is_position_reducing(sell, 0.0));
    // When strategy is long 5, sell order reduces it
    EXPECT_TRUE(is_position_reducing(sell, 5.0));
}

TEST(DefaultExitPolicy, ZeroPctsYieldEmpty)
{
    auto o = make_order(order_side::buy, 10, 100);
    default_exit_params p;
    p.mode = exit_policy_mode::floor;
    p.sl_pct = 0;
    p.tp_pct = 0;
    auto out = apply_default_exit_policy(p, o, 0.0, {});
    EXPECT_TRUE(out.empty());
}

TEST(DefaultExitPolicy, ParseMode)
{
    EXPECT_EQ(*parse_exit_policy_mode("floor"), exit_policy_mode::floor);
    EXPECT_EQ(*parse_exit_policy_mode("strategy_only"), exit_policy_mode::strategy_only);
    EXPECT_EQ(*parse_exit_policy_mode("engine-only"), exit_policy_mode::engine_only);
    EXPECT_EQ(*parse_exit_policy_mode("union"), exit_policy_mode::union_mode);
    EXPECT_FALSE(parse_exit_policy_mode("nope").has_value());
}

TEST(DefaultExitPolicy, IsPositionReducing)
{
    auto buy = make_order(order_side::buy, 1, 100);
    auto sell = make_order(order_side::sell, 1, 100);
    EXPECT_FALSE(is_position_reducing(buy, 0.0));
    EXPECT_FALSE(is_position_reducing(sell, 0.0));
    EXPECT_TRUE(is_position_reducing(sell, 5.0));
    EXPECT_TRUE(is_position_reducing(buy, -5.0));
    EXPECT_FALSE(is_position_reducing(buy, 5.0)); // add to long
}

TEST(DefaultExitPolicy, FloorDoesNotInjectTpWhenStrategyHasSlOnly)
{
    auto o = make_order(order_side::buy, 5, 200);
    exit_intent strat;
    strat.symbol = "TEST";
    strat.close_side = order_side::sell;
    strat.qty = 5;
    strat.stop_loss = 190.0; // SL only (e.g. ATR-based stop)

    default_exit_params p;
    p.mode = exit_policy_mode::floor;
    p.sl_pct = 0.01;
    p.tp_pct = 0.02; // Platform configured with TP 2%

    auto out = apply_default_exit_policy(p, o, 0.0, {strat});
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].stop_loss.has_value());
    EXPECT_DOUBLE_EQ(*out[0].stop_loss, 190.0);
    EXPECT_FALSE(out[0].take_profit.has_value()); // TP must NOT be injected
}

TEST(DefaultExitPolicy, FloorDoesNotInjectSlWhenStrategyHasTpOnly)
{
    auto o = make_order(order_side::buy, 5, 200);
    exit_intent strat;
    strat.symbol = "TEST";
    strat.close_side = order_side::sell;
    strat.qty = 5;
    strat.take_profit = 220.0; // TP only

    default_exit_params p;
    p.mode = exit_policy_mode::floor;
    p.sl_pct = 0.01;
    p.tp_pct = 0.02;

    auto out = apply_default_exit_policy(p, o, 0.0, {strat});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FALSE(out[0].stop_loss.has_value()); // SL must NOT be injected
    ASSERT_TRUE(out[0].take_profit.has_value());
    EXPECT_DOUBLE_EQ(*out[0].take_profit, 220.0);
}

TEST(DefaultExitPolicy, EmptyStrategyYieldsNoExitsWhenPlatformPctsZero)
{
    auto o = make_order(order_side::buy, 10, 100);
    default_exit_params p;
    p.mode = exit_policy_mode::floor;
    p.sl_pct = 0.0;
    p.tp_pct = 0.0;
    p.trail_pct = 0.0;

    auto out = apply_default_exit_policy(p, o, 0.0, {});
    EXPECT_TRUE(out.empty());
}
