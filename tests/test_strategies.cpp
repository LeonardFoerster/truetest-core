#include <gtest/gtest.h>

#include <algorithm>
#include "strategy/sma/sma_strategy.h"
#include "strategy/mean_reversion/mean_reversion_strategy.h"
#include "strategy/ma_crossover/ma_crossover_strategy.h"
#include "strategy/breakout/breakout_strategy.h"
#include "strategy/larry_connor/larry_connor_strategy.h"
#include "strategy/structure_continuation/structure_continuation_strategy.h"
#include "strategy/strategy_interface.h"
#include "strategy/strategy_registry.h"
#include "strategy/strategy_factory.h"

REGISTER_STRATEGY("test_macro_strategy_a", []() {
    return std::shared_ptr<IStrategy>{};
});
REGISTER_STRATEGY("test_macro_strategy_b", []() {
    return std::shared_ptr<IStrategy>{};
});

TEST(StrategyRegistry, MacroSupportsMultipleRegistrationsInOneTranslationUnit)
{
    EXPECT_TRUE(StrategyRegistry::instance().has("test_macro_strategy_a"));
    EXPECT_TRUE(StrategyRegistry::instance().has("test_macro_strategy_b"));
}

TEST(StrategyRegistry, AdaptiveHybridPrototypeIsNotShipped)
{
    EXPECT_FALSE(StrategyRegistry::instance().has("adaptive-hybrid"));
    EXPECT_THROW(StrategyRegistry::instance().create("adaptive-hybrid"),
                 std::runtime_error);
    EXPECT_THROW(StrategyFactory::create("adaptive-hybrid"),
                 std::runtime_error);
}

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
    // Now close > SMA -> buy
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
    // close < SMA -> sell
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

TEST(SmaStrategy, OptimisticLockBlocksUntilEngineFlats)
{
    // Emitting a buy locks the gate even before set_position_open(true)
    // from the engine, so subsequent bars above SMA cannot free-fire.
    sma_strategy s(3);
    s.set_position_open("TEST", false);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));

    auto first = s.on_market(make_mkt(3, 110.0));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->get_side(), order_side::buy);
    EXPECT_EQ(first->get_order_type(), order_type::market);

    // Do NOT call set_position_open — optimistic lock must still hold.
    for (int i = 0; i < 10; ++i)
        EXPECT_FALSE(s.on_market(make_mkt(4 + i, 111.0 + i)).has_value()) << "bar " << i;
}

TEST(SmaStrategy, FillStyleLimitAtClose)
{
    sma_strategy s(3);
    s.set_param("fill_style", 1.0);
    s.set_position_open("TEST", false);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));
    auto order = s.on_market(make_mkt(3, 110.0));
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->get_order_type(), order_type::limit);
    EXPECT_DOUBLE_EQ(order->get_price(), 110.0);
}

// --- Mean Reversion Strategy ---

TEST(MeanRevStrategy, BuySignal)
{
    mean_reversion_strategy s(3);
    s.set_position_open("TEST", false);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));
    // close < SMA -> buy (opposite of SMA)
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
    // Even if close < SMA, no second entry while a position is open -
    // the active bracket owns the lifecycle of the existing lot.
    mean_reversion_strategy s(3);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));
    s.set_position_open("TEST", true);
    auto order = s.on_market(make_mkt(3, 90.0));
    EXPECT_FALSE(order.has_value());
}

TEST(MeanRevStrategy, NoFreeFireWhileStillBelowSma)
{
    // Performance/correctness gate: after the first below-SMA edge, further
    // bars that remain below the mean must not emit orders or exit intents.
    mean_reversion_strategy s(3);
    s.set_param("exit_style", 0.0); // pct — deterministic SL/TP
    s.set_position_open("TEST", false);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));

    auto first = s.on_market(make_mkt(3, 90.0)); // edge into below
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->get_side(), order_side::buy);
    auto intents1 = s.take_pending_exit_intents();
    EXPECT_FALSE(intents1.empty());

    // Still below SMA, gate locked optimistically — no free-fire.
    for (int i = 0; i < 20; ++i)
    {
        auto o = s.on_market(make_mkt(4 + i, 88.0 - i * 0.1));
        EXPECT_FALSE(o.has_value()) << "bar " << i;
        EXPECT_TRUE(s.take_pending_exit_intents().empty()) << "bar " << i;
    }
}

TEST(MeanRevStrategy, ReentryRequiresFreshSmaCross)
{
    // After flat while still on the same side of the SMA, no re-entry until
    // price visits the other side and crosses back (edge re-arm).
    mean_reversion_strategy s(3);
    s.set_param("exit_style", 0.0);
    s.set_position_open("TEST", false);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));

    ASSERT_TRUE(s.on_market(make_mkt(3, 90.0)).has_value()); // long edge
    (void)s.take_pending_exit_intents();

    // Simulate stop-out / flat while still below the mean.
    s.set_position_open("TEST", false);

    // Still below — must NOT re-fire (no free-fire after flat).
    EXPECT_FALSE(s.on_market(make_mkt(4, 89.0)).has_value());
    EXPECT_TRUE(s.take_pending_exit_intents().empty());

    // Cross above SMA to re-arm the below-side edge (and may open a short).
    auto short_edge = s.on_market(make_mkt(5, 120.0));
    ASSERT_TRUE(short_edge.has_value());
    EXPECT_EQ(short_edge->get_side(), order_side::sell);
    (void)s.take_pending_exit_intents();
    s.set_position_open("TEST", false);

    // Cross back below — fresh long edge allowed.
    auto reentry = s.on_market(make_mkt(6, 80.0));
    ASSERT_TRUE(reentry.has_value());
    EXPECT_EQ(reentry->get_side(), order_side::buy);
}

TEST(MeanRevStrategy, OptimisticLockBlocksUntilEngineFlats)
{
    // Emitting an order locks the gate even before set_position_open(true)
    // from the engine, so the next bar cannot spam another entry.
    mean_reversion_strategy s(3);
    s.set_param("exit_style", 0.0);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 100.0));
    s.on_market(make_mkt(2, 100.0));

    ASSERT_TRUE(s.on_market(make_mkt(3, 90.0)).has_value());
    (void)s.take_pending_exit_intents();

    // Do NOT call set_position_open — optimistic lock must still hold.
    EXPECT_FALSE(s.on_market(make_mkt(4, 85.0)).has_value());
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
    // Prices: 100, 90, 80 -> slow ready, fast ready. fast=(90+80)/2=85, slow=(100+90+80)/3=90
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 90.0));
    s.on_market(make_mkt(2, 80.0));
    // Both ready, fast(85) < slow(90). First bar with both ready -> sets prev_fast_above=false, no signal.

    // Now spike: fast crosses above slow
    auto order = s.on_market(make_mkt(3, 120.0));
    // fast = (80+120)/2 = 100, slow = (90+80+120)/3 = 96.67, fast > slow, was below -> golden cross
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
    // fast=(90+100)/2=95, slow=(80+90+100)/3=90. fast>slow -> sets prev=true

    // Drop: fast crosses below slow
    auto order = s.on_market(make_mkt(3, 70.0));
    // fast=(100+70)/2=85, slow=(90+100+70)/3=86.67, fast<slow, was above -> death cross
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->get_side(), order_side::sell);
    EXPECT_EQ(order->get_order_type(), order_type::market);
}

TEST(MACrossoverStrategy, OptimisticLockBlocksUntilEngineFlats)
{
    // Golden cross locks the gate without set_position_open from the engine,
    // so a later golden cross (after a death cross in the MA state) cannot
    // stack another entry while the first is still unfilled.
    ma_crossover_strategy s(2, 3);
    s.set_position_open("TEST", false);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 90.0));
    s.on_market(make_mkt(2, 80.0)); // seeds prev_fast_above=false

    auto buy = s.on_market(make_mkt(3, 120.0)); // golden cross
    ASSERT_TRUE(buy.has_value());
    EXPECT_EQ(buy->get_side(), order_side::buy);
    EXPECT_EQ(buy->get_order_type(), order_type::market);

    // Do NOT call set_position_open — optimistic lock holds through death
    // then another recovery: death-cross sell is allowed (closes gate), but
    // wait — with optimistic open=true, death cross can emit sell. After
    // sell, gate is false again. For free-fire we care about bars that stay
    // above without a death cross:
    EXPECT_FALSE(s.on_market(make_mkt(4, 121.0)).has_value()); // still above, no re-entry
    EXPECT_FALSE(s.on_market(make_mkt(5, 122.0)).has_value());
}

TEST(MACrossoverStrategy, FillStyleLimitAtClose)
{
    ma_crossover_strategy s(2, 3);
    s.set_param("fill_style", 1.0);
    s.set_position_open("TEST", false);
    s.on_market(make_mkt(0, 100.0));
    s.on_market(make_mkt(1, 90.0));
    s.on_market(make_mkt(2, 80.0));
    auto order = s.on_market(make_mkt(3, 120.0));
    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->get_order_type(), order_type::limit);
    EXPECT_DOUBLE_EQ(order->get_price(), 120.0);
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
    // Risk-sized: equity*risk_fraction / (entry*sl_pct), capped by full equity
    // notional → min(606.06…, 10000/110) = 10000/110 ≈ 90.909…
    EXPECT_NEAR(order->get_quantity(), 10000.0 / 110.0, 1e-6);
    EXPECT_GT(order->get_quantity(), 0.0);
    // Default: market for classical next-open fill under exec_bar_delay.
    EXPECT_EQ(order->get_order_type(), order_type::market);
    EXPECT_DOUBLE_EQ(order->get_price(), 110.0); // signal reference still close
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

TEST(Strategy, AccountEquityHookUpdatesEveryEquitySizedBuiltin)
{
    auto expect_equity = [](IStrategy& strategy, double expected) {
        strategy.set_account_equity(expected);
        const auto schema = strategy.get_param_schema();
        const auto it = std::find_if(schema.begin(), schema.end(), [](const param_def& p) {
            return p.name == "equity";
        });
        ASSERT_NE(it, schema.end());
        EXPECT_DOUBLE_EQ(it->default_value, expected);
    };

    sma_strategy sma;
    ma_crossover_strategy ma;
    mean_reversion_strategy mean_reversion;
    breakout_strategy breakout;
    larry_connor_strategy larry;
    structure_continuation_strategy structure;

    expect_equity(sma, 11000.0);
    expect_equity(ma, 12000.0);
    expect_equity(mean_reversion, 13000.0);
    expect_equity(breakout, 14000.0);
    expect_equity(larry, 15000.0);
    expect_equity(structure, 16000.0);
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

TEST(BreakoutStrategy, BreakoutUsesOnlyPriorBarsForConsolidationBoundary)
{
    breakout_strategy s(/*equity=*/10000.0, /*risk_fraction=*/0.005,
                        /*atr_period=*/3, /*vol_period=*/3,
                        /*lookback=*/3, /*breakout_threshold=*/0.01,
                        /*atr_expansion=*/0.10, /*vol_mult=*/1.5,
                        /*min_rr=*/2.0);

    for (int i = 0; i < 5; ++i)
    {
        market_event flat(epoch_ms(i * 1000), "TEST",
                          100.0, 100.5, 99.5, 100.0, 100);
        EXPECT_EQ(s.on_market(flat), std::nullopt);
    }

    market_event breakout(epoch_ms(6000), "TEST",
                          100.0, 103.0, 99.8, 102.5, 1000);
    const auto order = s.on_market(breakout);
    ASSERT_TRUE(order.has_value())
        << "candidate high/volume/ATR must not redefine its own baseline";
    EXPECT_EQ(order->get_side(), order_side::buy);
    EXPECT_GT(order->get_quantity(), 0.0);
}

TEST(BreakoutStrategy, VolumeGateUsesBaseUnitsAcrossMixedScales)
{
    breakout_strategy integer_series(
        10000.0, 0.005, 3, 3, 3, 0.01, 0.10, 1.5, 2.0);
    breakout_strategy mixed_scale_series(
        10000.0, 0.005, 3, 3, 3, 0.01, 0.10, 1.5, 2.0);

    for (int i = 0; i < 5; ++i)
    {
        const auto ts = epoch_ms(i * 1000);
        ASSERT_FALSE(integer_series.on_market(market_event(
            ts, "TEST", 100.0, 100.5, 99.5, 100.0, 100, 1)));
        const bool fractional_encoding = (i % 2) != 0;
        const int64_t raw_volume = fractional_encoding
            ? 10'000'000'000LL : 100;
        const std::uint64_t scale = fractional_encoding
            ? 100'000'000ULL : 1ULL;
        ASSERT_FALSE(mixed_scale_series.on_market(market_event(
            ts, "TEST", 100.0, 100.5, 99.5, 100.0,
            raw_volume, scale)));
    }

    const auto ts = epoch_ms(6000);
    const auto integer_order = integer_series.on_market(market_event(
        ts, "TEST", 100.0, 103.0, 99.8, 102.5, 1000, 1));
    const auto scaled_order = mixed_scale_series.on_market(market_event(
        ts, "TEST", 100.0, 103.0, 99.8, 102.5,
        100'000'000'000LL, 100'000'000ULL));

    ASSERT_TRUE(integer_order);
    ASSERT_TRUE(scaled_order);
    EXPECT_DOUBLE_EQ(integer_order->get_quantity(),
                     scaled_order->get_quantity());
}
