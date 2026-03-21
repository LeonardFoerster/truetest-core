#include <gtest/gtest.h>
#include "strategy/sma_strategy.h"
#include "strategy/mean_reversion_strategy.h"
#include "strategy/ma_crossover_strategy.h"

static auto epoch_ms(int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

static market_event make_mkt(int64_t ms, double close)
{
    return market_event(epoch_ms(ms), "TEST", close, close, close, close, 100);
}

// --- SMA Strategy ---

TEST(SmaStrategy, NoSignalDuringWarmup)
{
    sma_strategy s(3);
    EXPECT_EQ(s.on_market(make_mkt(0, 100.0)), std::nullopt);
    EXPECT_EQ(s.on_market(make_mkt(1, 101.0)), std::nullopt);
}

TEST(SmaStrategy, BuySignal)
{
    sma_strategy s(3);
    s.set_position_open("TEST", false);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0)); // SMA = 100
    // Now close > SMA → buy
    auto order = s.on_market(make_mkt(3, 110.0)); // SMA ≈ 103.3, close=110 > SMA
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->get_side(), order_side::buy);
}

TEST(SmaStrategy, SellSignal)
{
    sma_strategy s(3);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));
    s.set_position_open("TEST", true);
    // close < SMA → sell
    auto order = s.on_market(make_mkt(3, 90.0)); // SMA ≈ 96.7, close=90 < SMA
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->get_side(), order_side::sell);
}

TEST(SmaStrategy, NoDoubleEntry)
{
    sma_strategy s(3);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));
    s.set_position_open("TEST", true);
    // close > SMA but already in position
    auto order = s.on_market(make_mkt(3, 110.0));
    EXPECT_EQ(order, std::nullopt);
}

// --- Mean Reversion Strategy ---

TEST(MeanRevStrategy, BuySignal)
{
    mean_reversion_strategy s(3);
    s.set_position_open("TEST", false);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));
    // close < SMA → buy (opposite of SMA)
    auto order = s.on_market(make_mkt(3, 90.0)); // SMA ≈ 96.7, close=90 < SMA
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->get_side(), order_side::buy);
}

TEST(MeanRevStrategy, SellSignal)
{
    mean_reversion_strategy s(3);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));
    s.set_position_open("TEST", true);
    // close > SMA → sell (opposite of SMA)
    auto order = s.on_market(make_mkt(3, 110.0));
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->get_side(), order_side::sell);
}

// --- MA Crossover Strategy ---

TEST(MACrossoverStrategy, BuySignal)
{
    ma_crossover_strategy s(3);
    s.set_position_open("TEST", false);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));
    auto order = s.on_market(make_mkt(3, 110.0));
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->get_side(), order_side::buy);
}

// --- Common Checks ---

TEST(Strategy, OrderFields)
{
    sma_strategy s(3);
    s.set_position_open("TEST", false);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));
    auto order = s.on_market(make_mkt(3, 110.0));
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->get_symbol(), "TEST");
    EXPECT_EQ(order->get_quantity(), 100);
    EXPECT_EQ(order->get_order_type(), order_type::limit);
    EXPECT_DOUBLE_EQ(order->get_price(), 110.0);
}

TEST(Strategy, OnTickDefault)
{
    sma_strategy s(3);
    tick_event t(epoch_ms(0), "X", 100.0, 1, tick_side::bid);
    EXPECT_EQ(s.on_tick(t), std::nullopt);
}

TEST(Strategy, OnL2UpdateDefault)
{
    sma_strategy s(3);
    l2_update_event e(epoch_ms(0), "X", tick_side::bid, 100.0, 50);
    EXPECT_EQ(s.on_l2_update(e), std::nullopt);
}
