#include <gtest/gtest.h>
#include "strategy/sma_strategy.h"
#include "strategy/mean_reversion_strategy.h"
#include "strategy/ma_crossover_strategy.h"
#include "strategy/breakout_strategy.h"

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

TEST(MeanRevStrategy, NoSignalSellWhenAboveSma)
{
    // Exits are owned by the engine's bracket (SL/TP), not by a SMA-cross
    // signal. While position is open, the strategy must stay silent so SL
    // and TP are the only ways out.
    mean_reversion_strategy s(3);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));
    s.set_position_open("TEST", true);
    auto order = s.on_market(make_mkt(3, 110.0));
    EXPECT_FALSE(order.has_value());
}

TEST(MeanRevStrategy, NoReentryWhilePositionOpen)
{
    // Even if close < SMA, no second entry while a position is open —
    // the active bracket owns the lifecycle of the existing lot.
    mean_reversion_strategy s(3);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));
    s.set_position_open("TEST", true);
    auto order = s.on_market(make_mkt(3, 90.0));
    EXPECT_FALSE(order.has_value());
}

// --- MA Crossover Strategy ---

TEST(MACrossoverStrategy, NoSignalDuringWarmup)
{
    // fast=2, slow=4
    ma_crossover_strategy s(2, 4);
    s.set_position_open("TEST", false);
    // Need at least slow_period (4) prices + 1 for crossover detection
    EXPECT_EQ(s.on_market(make_mkt(0, 100.0)), std::nullopt);
    EXPECT_EQ(s.on_market(make_mkt(1, 100.0)), std::nullopt);
    EXPECT_EQ(s.on_market(make_mkt(2, 100.0)), std::nullopt);
}

TEST(MACrossoverStrategy, BuyOnCrossover)
{
    // fast=2, slow=3
    ma_crossover_strategy s(2, 3);
    s.set_position_open("TEST", false);
    // Prices: 100, 90, 80 → slow ready, fast ready. fast=(90+80)/2=85, slow=(100+90+80)/3=90
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 90.0));
    s.on_market(make_mkt(2, 80.0));
    // Both ready, fast(85) < slow(90). First bar with both ready → sets prev_fast_above=false, no signal.

    // Now spike: fast crosses above slow
    auto order = s.on_market(make_mkt(3, 120.0));
    // fast = (80+120)/2 = 100, slow = (90+80+120)/3 = 96.67, fast > slow, was below → golden cross
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->get_side(), order_side::buy);
}

TEST(MACrossoverStrategy, DeathCross)
{
    // fast=2, slow=3
    ma_crossover_strategy s(2, 3);
    s.set_position_open("TEST", true);
    // Build up: rising so fast > slow
    s.on_market(make_mkt(0, 80.0));
    s.on_market(make_mkt(1, 90.0));
    s.on_market(make_mkt(2, 100.0));
    // fast=(90+100)/2=95, slow=(80+90+100)/3=90. fast>slow → sets prev=true

    // Drop: fast crosses below slow
    auto order = s.on_market(make_mkt(3, 70.0));
    // fast=(100+70)/2=85, slow=(90+100+70)/3=86.67, fast<slow, was above → death cross
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->get_side(), order_side::sell);
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

// --- Breakout Strategy (Coiled Spring) ---

TEST(BreakoutStrategy, WarmupNoSignal)
{
    breakout_strategy s(10000.0, 0.005);
    s.set_position_open("TEST", false);
    // Need ~14 ATR + 20 vol bars
    for (int i = 0; i < 25; ++i)
    {
        auto m = market_event(epoch_ms(i * 1000), "TEST", 100.0, 101.0, 99.0, 100.0, 1000);
        EXPECT_EQ(s.on_market(m), std::nullopt);
    }
}

TEST(BreakoutStrategy, ParamsSchemaHasBreakoutKeys)
{
    breakout_strategy s;
    auto schema = s.get_param_schema();
    bool has_equity = false;
    for (const auto& p : schema) if (p.name == "equity") has_equity = true;
    EXPECT_TRUE(has_equity);
    EXPECT_TRUE(s.get_indicator_values("FOO").empty());
}
