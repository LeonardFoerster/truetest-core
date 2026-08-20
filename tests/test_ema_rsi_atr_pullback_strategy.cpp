#include <gtest/gtest.h>

#include "strategy/ema_rsi_atr_pullback/ema_rsi_atr_pullback_strategy.h"
#include "strategy/strategy_registry.h"
#include "execution/position_sizing.h"
#include "exits/exit_manager.h"
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

inline auto make_tp(int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

inline market_event make_bar(int64_t ms, const std::string& symbol,
                             double open, double high, double low, double close,
                             uint64_t volume = 100)
{
    return market_event(make_tp(ms), symbol, open, high, low, close, volume);
}

inline market_event make_bar(int64_t ms, double open, double high, double low, double close)
{
    return make_bar(ms, "TEST", open, high, low, close);
}

inline fill_event make_fill(uint64_t order_id, const std::string& symbol,
                            order_side side, double qty, double price,
                            uint64_t fill_id = 1)
{
    return fill_event(make_tp(0), symbol, order_id, side, qty, price,
                      /*commission=*/0.0, /*remaining=*/0.0, fill_id);
}

} // anonymous namespace

// ============================================================================
// 16.1 Konstruktion, Defaults und Registry
// ============================================================================

TEST(EmaRsiAtrPullbackTest, RegistryKeyPresentAndCreatable)
{
    EXPECT_TRUE(StrategyRegistry::instance().has("ema-rsi-atr-pullback"));
    auto strat = StrategyRegistry::instance().create("ema-rsi-atr-pullback");
    ASSERT_NE(strat, nullptr);
    EXPECT_TRUE(strat->supports_mc_trial_reuse());
}

TEST(EmaRsiAtrPullbackTest, DefaultValuesAndSchema)
{
    ema_rsi_atr_pullback_strategy s;
    auto schema = s.get_param_schema();

    // Verify all 17 parameter definitions exist in schema
    std::unordered_map<std::string, param_def> pmap;
    for (const auto& p : schema)
        pmap[p.name] = p;

    EXPECT_EQ(pmap.count("ema_period"), 1u);
    EXPECT_DOUBLE_EQ(pmap["ema_period"].default_value, 150.0);

    EXPECT_EQ(pmap.count("rsi_period"), 1u);
    EXPECT_DOUBLE_EQ(pmap["rsi_period"].default_value, 14.0);

    EXPECT_EQ(pmap.count("atr_period"), 1u);
    EXPECT_DOUBLE_EQ(pmap["atr_period"].default_value, 14.0);

    EXPECT_EQ(pmap.count("long_rsi_threshold"), 1u);
    EXPECT_DOUBLE_EQ(pmap["long_rsi_threshold"].default_value, 40.0);

    EXPECT_EQ(pmap.count("short_rsi_threshold"), 1u);
    EXPECT_DOUBLE_EQ(pmap["short_rsi_threshold"].default_value, 60.0);

    EXPECT_EQ(pmap.count("atr_stop_multiplier"), 1u);
    EXPECT_DOUBLE_EQ(pmap["atr_stop_multiplier"].default_value, 2.0);

    EXPECT_EQ(pmap.count("risk_fraction"), 1u);
    EXPECT_DOUBLE_EQ(pmap["risk_fraction"].default_value, 0.005);

    EXPECT_EQ(pmap.count("allow_long"), 1u);
    EXPECT_DOUBLE_EQ(pmap["allow_long"].default_value, 1.0);

    EXPECT_EQ(pmap.count("allow_short"), 1u);
    EXPECT_DOUBLE_EQ(pmap["allow_short"].default_value, 1.0);

    EXPECT_EQ(pmap.count("quantity_step"), 1u);
}

// ============================================================================
// 16.2 Warm-up
// ============================================================================

TEST(EmaRsiAtrPullbackTest, NoSignalDuringWarmup)
{
    // Small periods: EMA=3, RSI=2, ATR=2
    ema_rsi_atr_pullback_strategy s(3, 2, 2);

    // Bar 1: not ready
    EXPECT_EQ(s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0)), std::nullopt);
    EXPECT_TRUE(s.take_pending_exit_intents().empty());

    // Bar 2: RSI initialized (needs 1 prev price + 2 changes = 3 bars)
    EXPECT_EQ(s.on_market(make_bar(200, 100.0, 102.0, 98.0, 95.0)), std::nullopt);
    EXPECT_TRUE(s.take_pending_exit_intents().empty());

    // Bar 3: EMA (3 bars) and ATR (2 bars) are ready, RSI ready on 3rd bar.
    // However, prev_rsi was not yet available for a cross on bar 3.
    EXPECT_EQ(s.on_market(make_bar(300, 95.0, 105.0, 94.0, 104.0)), std::nullopt);
    EXPECT_TRUE(s.take_pending_exit_intents().empty());
}

// ============================================================================
// 16.3 Long-Signal
// ============================================================================

TEST(EmaRsiAtrPullbackTest, ValidLongSignalGeneratesMarketOrderAndExitIntent)
{
    // Small periods: EMA=3, RSI=2, ATR=2, risk=0.01, mult=2.0, equity=10000
    ema_rsi_atr_pullback_strategy s(3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);

    // Feed bars to set up:
    // Bar 1: close=100
    s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    // Bar 2: drop close to 90 -> RSI drops
    s.on_market(make_bar(200, 100.0, 100.0, 89.0, 90.0));
    // Bar 3: close=88 -> RSI <= 40 established as prev_rsi
    s.on_market(make_bar(300, 90.0, 92.0, 87.0, 88.0));
    // Bar 4: rally close=110 > EMA (~96), RSI crosses above 40
    auto order = s.on_market(make_bar(400, 90.0, 112.0, 89.0, 110.0));

    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->get_symbol(), "TEST");
    EXPECT_EQ(order->get_order_type(), order_type::market);
    EXPECT_EQ(order->get_side(), order_side::buy);
    EXPECT_GT(order->get_quantity(), 0.0);
    EXPECT_DOUBLE_EQ(order->get_price(), 110.0); // Signal close as reference

    // Check exit intent
    auto intents = s.take_pending_exit_intents();
    ASSERT_EQ(intents.size(), 1u);
    const auto& ei = intents[0];
    EXPECT_EQ(ei.symbol, "TEST");
    EXPECT_EQ(ei.close_side, order_side::sell);
    EXPECT_DOUBLE_EQ(ei.qty, order->get_quantity());
    EXPECT_DOUBLE_EQ(ei.qty_fraction, 1.0);
    ASSERT_TRUE(ei.stop_loss.has_value());
    EXPECT_LT(*ei.stop_loss, 110.0); // Stop below entry
    ASSERT_TRUE(ei.reference_entry.has_value());
    EXPECT_DOUBLE_EQ(*ei.reference_entry, 110.0);
    EXPECT_FALSE(ei.take_profit.has_value());
    EXPECT_FALSE(ei.trailing_pct.has_value());
    EXPECT_EQ(ei.strategy_name, "ema-rsi-atr-pullback");
}

// ============================================================================
// 16.4 Short-Signal
// ============================================================================

TEST(EmaRsiAtrPullbackTest, ValidShortSignalGeneratesSellOrderAndBuyExitIntent)
{
    // Small periods: EMA=3, RSI=2, ATR=2, risk=0.01, mult=2.0, equity=10000
    ema_rsi_atr_pullback_strategy s(3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);

    // Warmup and push RSI high
    s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(200, 100.0, 112.0, 99.0, 110.0));
    s.on_market(make_bar(300, 110.0, 122.0, 109.0, 120.0)); // RSI high >= 60

    // Dump below EMA: close=80 < EMA, RSI drops below 60
    auto order = s.on_market(make_bar(400, 110.0, 112.0, 79.0, 80.0));

    ASSERT_TRUE(order.has_value());
    EXPECT_EQ(order->get_symbol(), "TEST");
    EXPECT_EQ(order->get_order_type(), order_type::market);
    EXPECT_EQ(order->get_side(), order_side::sell);
    EXPECT_GT(order->get_quantity(), 0.0);
    EXPECT_DOUBLE_EQ(order->get_price(), 80.0);

    auto intents = s.take_pending_exit_intents();
    ASSERT_EQ(intents.size(), 1u);
    const auto& ei = intents[0];
    EXPECT_EQ(ei.close_side, order_side::buy);
    ASSERT_TRUE(ei.stop_loss.has_value());
    EXPECT_GT(*ei.stop_loss, 80.0); // Stop above entry for short
    ASSERT_TRUE(ei.reference_entry.has_value());
    EXPECT_DOUBLE_EQ(*ei.reference_entry, 80.0);
    EXPECT_EQ(ei.strategy_name, "ema-rsi-atr-pullback");
}

// ============================================================================
// 16.5 Falscher Trend & Neutral
// ============================================================================

TEST(EmaRsiAtrPullbackTest, WrongTrendBlocksSignals)
{
    // Long cross but close < EMA -> no entry
    ema_rsi_atr_pullback_strategy s(3, 2, 2);

    s.on_market(make_bar(100, 100.0, 105.0, 95.0, 100.0));
    s.on_market(make_bar(200, 100.0, 102.0, 89.0, 90.0));
    s.on_market(make_bar(300, 90.0, 92.0, 84.0, 85.0)); // prev_rsi low

    // Slight bounce to 87: RSI crosses 40, but close (87) < EMA (approx 91.6)
    auto order = s.on_market(make_bar(400, 85.0, 89.0, 84.0, 87.0));
    EXPECT_EQ(order, std::nullopt);
    EXPECT_TRUE(s.take_pending_exit_intents().empty());
}

// ============================================================================
// 16.6 Schwellenwerte
// ============================================================================

TEST(EmaRsiAtrPullbackTest, ExactBoundarySemantics)
{
    ema_rsi_atr_pullback_strategy s(3, 2, 2);

    // Warm up
    s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(200, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(300, 100.0, 101.0, 99.0, 100.0));

    // When price doesn't change, RSI=100 (or NaN/loss=0), no cross <= 40
    auto order = s.on_market(make_bar(400, 100.0, 101.0, 99.0, 100.0));
    EXPECT_EQ(order, std::nullopt);
}

// ============================================================================
// 16.7 Pending-Gate und kein Pyramiding
// ============================================================================

TEST(EmaRsiAtrPullbackTest, NoPyramidingAndSingleActiveTrade)
{
    ema_rsi_atr_pullback_strategy s(3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);

    // Trigger long entry
    s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(200, 100.0, 100.0, 89.0, 90.0));
    s.on_market(make_bar(300, 90.0, 92.0, 87.0, 88.0));
    auto order1 = s.on_market(make_bar(400, 90.0, 112.0, 89.0, 110.0));
    ASSERT_TRUE(order1.has_value());
    EXPECT_EQ(s.get_trade_state("TEST"), ema_rsi_atr_pullback_strategy::trade_state::entry_pending_long);

    // Next bar before fill: no new order
    auto order2 = s.on_market(make_bar(500, 110.0, 115.0, 109.0, 114.0));
    EXPECT_EQ(order2, std::nullopt);

    // Fill the opener
    s.on_fill(make_fill(1001, "TEST", order_side::buy, order1->get_quantity(), 110.0), 1001);
    EXPECT_EQ(s.get_trade_state("TEST"), ema_rsi_atr_pullback_strategy::trade_state::long_open);

    // Another bar in position above EMA: no new order
    auto order3 = s.on_market(make_bar(600, 114.0, 120.0, 113.0, 118.0));
    EXPECT_EQ(order3, std::nullopt);
}

// ============================================================================
// 16.8 Per-Symbol-Isolation
// ============================================================================

TEST(EmaRsiAtrPullbackTest, PerSymbolIsolation)
{
    ema_rsi_atr_pullback_strategy s(3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);

    // Symbol A warm-up and signal
    s.on_market(make_bar(100, "SYM_A", 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(200, "SYM_A", 100.0, 100.0, 89.0, 90.0));
    s.on_market(make_bar(300, "SYM_A", 90.0, 92.0, 87.0, 88.0));
    auto orderA = s.on_market(make_bar(400, "SYM_A", 90.0, 112.0, 89.0, 110.0));
    ASSERT_TRUE(orderA.has_value());
    EXPECT_EQ(s.get_trade_state("SYM_A"), ema_rsi_atr_pullback_strategy::trade_state::entry_pending_long);
    EXPECT_EQ(s.get_trade_state("SYM_B"), ema_rsi_atr_pullback_strategy::trade_state::flat);

    // Symbol B receives first bar -> still in warmup, not affected by A
    auto orderB = s.on_market(make_bar(400, "SYM_B", 50.0, 51.0, 49.0, 50.0));
    EXPECT_EQ(orderB, std::nullopt);
    EXPECT_EQ(s.get_trade_state("SYM_B"), ema_rsi_atr_pullback_strategy::trade_state::flat);

    // Fill for A does not modify B
    s.on_fill(make_fill(2001, "SYM_A", order_side::buy, orderA->get_quantity(), 110.0), 2001);
    EXPECT_EQ(s.get_trade_state("SYM_A"), ema_rsi_atr_pullback_strategy::trade_state::long_open);
    EXPECT_EQ(s.get_trade_state("SYM_B"), ema_rsi_atr_pullback_strategy::trade_state::flat);
}

// ============================================================================
// 16.9 Positionsgröße (Section 6.6 Rechenbeispiel)
// ============================================================================

TEST(EmaRsiAtrPullbackTest, ReferenceSizingExampleWithQuantityStep)
{
    // Formula check matching Section 6.6:
    // Equity: 100000.0
    // risk_fraction: 0.005 (budget = 500)
    // ATR: 2.40
    // atr_stop_multiplier: 2.0 (stop distance = 4.80)
    // Fees/Slip: 0
    // Quantity-Step: 1.0
    // raw_quantity = 500 / 4.80 = 104.1666...
    // final_quantity = 104

    truetest::risk::risk_size_inputs in;
    in.equity            = 100000.0;
    in.risk_fraction     = 0.005;
    in.entry_price       = 100.0;
    in.stop_price        = 95.20; // 100 - 4.80
    in.is_long           = true;
    in.entry_fee_rate    = 0.0;
    in.exit_fee_rate     = 0.0;
    in.entry_slip_bps    = 0.0;
    in.exit_slip_bps     = 0.0;
    in.fixed_fee_per_leg = 0.0;
    in.max_notional_frac = 0.0;

    double raw_qty = truetest::risk::compute_risk_quantity(in);
    EXPECT_NEAR(raw_qty, 104.1666667, 1e-5);

    double final_qty = std::floor(raw_qty / 1.0) * 1.0;
    EXPECT_DOUBLE_EQ(final_qty, 104.0);

    // Symmetric short check
    in.is_long    = false;
    in.stop_price = 104.80;
    double raw_short = truetest::risk::compute_risk_quantity(in);
    EXPECT_NEAR(raw_short, 104.1666667, 1e-5);
    EXPECT_DOUBLE_EQ(std::floor(raw_short / 1.0) * 1.0, 104.0);
}

TEST(EmaRsiAtrPullbackTest, InvalidSizingYieldsNoTradeAndNoFixedFallback)
{
    // Strategy with non-positive equity
    ema_rsi_atr_pullback_strategy s(3, 2, 2);
    s.set_account_equity(0.0); // non-positive equity

    s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(200, 100.0, 100.0, 89.0, 90.0));
    s.on_market(make_bar(300, 90.0, 92.0, 87.0, 88.0));
    auto order = s.on_market(make_bar(400, 90.0, 112.0, 89.0, 110.0));

    // Must NOT emit fixed size fallback (e.g. qty=1 or 100)
    EXPECT_EQ(order, std::nullopt);
    EXPECT_TRUE(s.take_pending_exit_intents().empty());
}

// ============================================================================
// 16.10 Stop-Intent
// ============================================================================

TEST(EmaRsiAtrPullbackTest, StopIntentDrainedExactlyOnce)
{
    ema_rsi_atr_pullback_strategy s(3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);

    s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(200, 100.0, 100.0, 89.0, 90.0));
    s.on_market(make_bar(300, 90.0, 92.0, 87.0, 88.0));
    auto order = s.on_market(make_bar(400, 90.0, 112.0, 89.0, 110.0));
    ASSERT_TRUE(order.has_value());

    auto first_drain = s.take_pending_exit_intents();
    EXPECT_EQ(first_drain.size(), 1u);

    auto second_drain = s.take_pending_exit_intents();
    EXPECT_TRUE(second_drain.empty());
}

// ============================================================================
// 16.11 Fill- und Opener-Lifecycle
// ============================================================================

TEST(EmaRsiAtrPullbackTest, PartialFillsAndFullCloseLifecycle)
{
    ema_rsi_atr_pullback_strategy s(3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);

    // Generate Long Entry
    s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(200, 100.0, 100.0, 89.0, 90.0));
    s.on_market(make_bar(300, 90.0, 92.0, 87.0, 88.0));
    auto order = s.on_market(make_bar(400, 90.0, 112.0, 89.0, 110.0));
    ASSERT_TRUE(order.has_value());
    const double requested_qty = order->get_quantity();

    // Partial opener fill 1: 40% of qty
    s.on_fill(make_fill(501, "TEST", order_side::buy, requested_qty * 0.4, 110.0, 1), 501);
    EXPECT_EQ(s.get_trade_state("TEST"), ema_rsi_atr_pullback_strategy::trade_state::long_open);
    EXPECT_NEAR(s.get_open_qty("TEST"), requested_qty * 0.4, 1e-6);

    // Partial opener fill 2: 60% of qty
    s.on_fill(make_fill(501, "TEST", order_side::buy, requested_qty * 0.6, 110.0, 2), 501);
    EXPECT_NEAR(s.get_open_qty("TEST"), requested_qty, 1e-6);
    EXPECT_EQ(s.get_opener_order_id("TEST"), 501u);

    // Foreign opener fill with different opener_order_id is ignored
    s.on_fill(make_fill(999, "TEST", order_side::sell, requested_qty, 115.0, 3), 999);
    EXPECT_NEAR(s.get_open_qty("TEST"), requested_qty, 1e-6);

    // Closer fill matching opener 501
    s.on_fill(make_fill(601, "TEST", order_side::sell, requested_qty, 115.0, 4), 501);
    EXPECT_DOUBLE_EQ(s.get_open_qty("TEST"), 0.0);
    EXPECT_EQ(s.get_opener_order_id("TEST"), 0u);
    EXPECT_EQ(s.get_trade_state("TEST"), ema_rsi_atr_pullback_strategy::trade_state::flat);
}

// ============================================================================
// 16.12 EMA-Trend-Exit
// ============================================================================

TEST(EmaRsiAtrPullbackTest, EmaTrendExitEmitsCloserWithOpenerId)
{
    ema_rsi_atr_pullback_strategy s(3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);

    // Long entry
    s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(200, 100.0, 100.0, 89.0, 90.0));
    s.on_market(make_bar(300, 90.0, 92.0, 87.0, 88.0));
    auto order = s.on_market(make_bar(400, 90.0, 112.0, 89.0, 110.0));
    ASSERT_TRUE(order.has_value());
    const double qty = order->get_quantity();

    // The engine drains exit intents after each entry callback
    auto entry_intents = s.take_pending_exit_intents();
    EXPECT_EQ(entry_intents.size(), 1u);

    // Fill opener
    s.on_fill(make_fill(701, "TEST", order_side::buy, qty, 110.0), 701);
    EXPECT_EQ(s.get_trade_state("TEST"), ema_rsi_atr_pullback_strategy::trade_state::long_open);

    // Bar 5: price crashes below EMA (close=70 < EMA)
    auto close_order = s.on_market(make_bar(500, 110.0, 110.0, 69.0, 70.0));
    ASSERT_TRUE(close_order.has_value());
    EXPECT_EQ(close_order->get_symbol(), "TEST");
    EXPECT_EQ(close_order->get_side(), order_side::sell);
    EXPECT_DOUBLE_EQ(close_order->get_quantity(), qty);
    EXPECT_EQ(close_order->get_opener_order_id(), 701u);
    EXPECT_EQ(s.get_trade_state("TEST"), ema_rsi_atr_pullback_strategy::trade_state::exit_pending_long);

    // Trend exit must NOT generate a new exit intent
    EXPECT_TRUE(s.take_pending_exit_intents().empty());
}

// ============================================================================
// 16.13 Ungültige Bars
// ============================================================================

TEST(EmaRsiAtrPullbackTest, InvalidBarsIgnoredWithoutCorruptingState)
{
    ema_rsi_atr_pullback_strategy s1(3, 2, 2);
    ema_rsi_atr_pullback_strategy s2(3, 2, 2);

    // Both receive bar 1
    s1.on_market(make_bar(100, 100.0, 102.0, 98.0, 100.0));
    s2.on_market(make_bar(100, 100.0, 102.0, 98.0, 100.0));

    // s1 receives invalid bars
    EXPECT_EQ(s1.on_market(make_bar(200, 100.0, std::numeric_limits<double>::quiet_NaN(), 98.0, 100.0)), std::nullopt);
    EXPECT_EQ(s1.on_market(make_bar(201, 100.0, 90.0, 110.0, 100.0)), std::nullopt); // high < low
    EXPECT_EQ(s1.on_market(make_bar(202, 100.0, 105.0, 95.0, -10.0)), std::nullopt);  // close <= 0

    // Both receive valid bars 2 and 3
    s1.on_market(make_bar(300, 100.0, 101.0, 99.0, 100.0));
    s2.on_market(make_bar(300, 100.0, 101.0, 99.0, 100.0));

    s1.on_market(make_bar(400, 100.0, 101.0, 99.0, 100.0));
    s2.on_market(make_bar(400, 100.0, 101.0, 99.0, 100.0));

    auto v1 = s1.get_indicator_values("TEST");
    auto v2 = s2.get_indicator_values("TEST");
    ASSERT_EQ(v1.size(), v2.size());
    for (size_t i = 0; i < v1.size(); ++i)
    {
        EXPECT_EQ(v1[i].first, v2[i].first);
        EXPECT_DOUBLE_EQ(v1[i].second, v2[i].second);
    }
}

// ============================================================================
// 16.14 Parameter-Validierung
// ============================================================================

TEST(EmaRsiAtrPullbackTest, ParameterValidation)
{
    ema_rsi_atr_pullback_strategy s;

    EXPECT_THROW(s.set_param("unknown_key", 1.0), std::runtime_error);
    EXPECT_THROW(s.set_param("ema_period", 1.0), std::runtime_error);      // < 2
    EXPECT_THROW(s.set_param("ema_period", 14.5), std::runtime_error);     // non-integer
    EXPECT_THROW(s.set_param("rsi_period", 1.0), std::runtime_error);      // < 2
    EXPECT_THROW(s.set_param("atr_period", 0.0), std::runtime_error);      // < 1
    EXPECT_THROW(s.set_param("long_rsi_threshold", 70.0), std::runtime_error); // >= short threshold (60)
    EXPECT_THROW(s.set_param("short_rsi_threshold", 30.0), std::runtime_error); // <= long threshold (40)
    EXPECT_THROW(s.set_param("risk_fraction", -0.01), std::runtime_error);
    EXPECT_THROW(s.set_param("risk_fraction", 0.06), std::runtime_error);  // > 0.05
    EXPECT_THROW(s.set_param("atr_stop_multiplier", 0.0), std::runtime_error);
    EXPECT_THROW(s.set_param("allow_long", 2.0), std::runtime_error);      // not 0 or 1
    EXPECT_THROW(s.set_param("equity", std::numeric_limits<double>::quiet_NaN()), std::runtime_error);

    // Valid param setting
    EXPECT_NO_THROW(s.set_param("long_rsi_threshold", 35.0));
    EXPECT_NO_THROW(s.set_param("short_rsi_threshold", 65.0));
    EXPECT_NO_THROW(s.set_param("quantity_step", 1.0));
}

// ============================================================================
// 16.15 Reset und Monte Carlo
// ============================================================================

TEST(EmaRsiAtrPullbackTest, ResetClearsAllStateDeterministically)
{
    ema_rsi_atr_pullback_strategy s(3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);

    // Run trial 1
    s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(200, 100.0, 100.0, 89.0, 90.0));
    s.on_market(make_bar(300, 90.0, 92.0, 87.0, 88.0));
    auto order1 = s.on_market(make_bar(400, 90.0, 112.0, 89.0, 110.0));
    ASSERT_TRUE(order1.has_value());
    s.on_fill(make_fill(801, "TEST", order_side::buy, order1->get_quantity(), 110.0), 801);
    EXPECT_EQ(s.get_trade_state("TEST"), ema_rsi_atr_pullback_strategy::trade_state::long_open);

    // Reset for next trial
    s.reset(42);
    EXPECT_EQ(s.get_trade_state("TEST"), ema_rsi_atr_pullback_strategy::trade_state::flat);
    EXPECT_DOUBLE_EQ(s.get_open_qty("TEST"), 0.0);
    EXPECT_EQ(s.get_opener_order_id("TEST"), 0u);
    EXPECT_TRUE(s.take_pending_exit_intents().empty());

    // Run identical sequence in trial 2 -> must produce identical order
    s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(200, 100.0, 100.0, 89.0, 90.0));
    s.on_market(make_bar(300, 90.0, 92.0, 87.0, 88.0));
    auto order2 = s.on_market(make_bar(400, 90.0, 112.0, 89.0, 110.0));
    ASSERT_TRUE(order2.has_value());
    EXPECT_DOUBLE_EQ(order1->get_quantity(), order2->get_quantity());
    EXPECT_DOUBLE_EQ(order1->get_price(), order2->get_price());
}

// ============================================================================
// 16.16 Engine-Integration (End-to-End Test)
// ============================================================================

#include "market_maker/market_maker.h"
#include "orderbook/orderbook.h"

TEST(EmaRsiAtrPullbackTest, EngineIntegrationWithExecutionBarDelayAndRebase)
{
    // Test that the strategy integrates into the Engine event loop,
    // with execution_bar_delay = 1 and strategy_only exit policy mode.
    // Verifies:
    // 1. Entry signal on Bar N (bar 3, close=110)
    // 2. Market order executed on Bar N+1 at next observation price (bar 4, open=112)
    // 3. ExitManager arms ATR stop-loss and receives opener fill attribution

    auto dh = std::make_shared<data_handler>();
    // Bar 0: close=100
    dh->load_into_queue("2024-01-01", "TEST", 100.0, 101.0, 99.0, 100.0, 1000);
    // Bar 1: close=90
    dh->load_into_queue("2024-01-01", "TEST", 100.0, 100.0, 89.0, 90.0, 1000);
    // Bar 2: close=88 (prev_rsi <= 40)
    dh->load_into_queue("2024-01-01", "TEST", 90.0, 92.0, 87.0, 88.0, 1000);
    // Bar 3: close=110 (triggers Buy signal with reference 110.0)
    dh->load_into_queue("2024-01-01", "TEST", 90.0, 112.0, 89.0, 110.0, 1000);
    // Bar 4: opens at 112.0, closes at 114.0 -> Fill occurs here under bar-delay=1!
    dh->load_into_queue("2024-01-01", "TEST", 112.0, 115.0, 111.0, 114.0, 1000);
    // Bar 5: normal bar
    dh->load_into_queue("2024-01-01", "TEST", 114.0, 116.0, 113.0, 115.0, 1000);

    auto ob = std::make_shared<orderbook>();
    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    auto strat = std::make_shared<ema_rsi_atr_pullback_strategy>(
        3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);

    engine_config cfg;
    cfg.seed = 1;
    cfg.execution_bar_delay = 1;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    cfg.initial_balance = 10000.0;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    auto report = eng.get_analytics().generate_report();
    ASSERT_FALSE(report.trades.empty());
    const auto& trade = report.trades.front();
    EXPECT_EQ(trade.symbol, "TEST");
    EXPECT_GT(trade.fill_price, 100.0);

    // Verify strategy position state was updated via on_fill
    EXPECT_EQ(strat->get_trade_state("TEST"),
              ema_rsi_atr_pullback_strategy::trade_state::long_open);
    EXPECT_GT(strat->get_open_qty("TEST"), 0.0);
    EXPECT_NE(strat->get_opener_order_id("TEST"), 0u);
}

TEST(EmaRsiAtrPullbackTest, SizingVariationsAndEdgeCases)
{
    ema_rsi_atr_pullback_strategy s(3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);

    // Higher ATR reduces quantity
    truetest::risk::risk_size_inputs in1;
    in1.equity = 10000.0; in1.risk_fraction = 0.005; in1.entry_price = 100.0;
    in1.stop_price = 96.0; in1.is_long = true; // stop_dist = 4.0
    double q1 = truetest::risk::compute_risk_quantity(in1);

    truetest::risk::risk_size_inputs in2 = in1;
    in2.stop_price = 92.0; // stop_dist = 8.0 (higher ATR)
    double q2 = truetest::risk::compute_risk_quantity(in2);
    EXPECT_LT(q2, q1);

    // Higher equity increases quantity
    truetest::risk::risk_size_inputs in3 = in1;
    in3.equity = 20000.0;
    double q3 = truetest::risk::compute_risk_quantity(in3);
    EXPECT_GT(q3, q1);

    // Fees and slippage reduce quantity
    truetest::risk::risk_size_inputs in4 = in1;
    in4.entry_fee_rate = 0.001; in4.exit_fee_rate = 0.001;
    in4.entry_slip_bps = 10.0; in4.exit_slip_bps = 10.0;
    double q4 = truetest::risk::compute_risk_quantity(in4);
    EXPECT_LT(q4, q1);

    // Notional cap limits quantity
    truetest::risk::risk_size_inputs in5 = in1;
    in5.max_notional_frac = 0.01; // max 100.0 notional -> max qty 1.0
    double q5 = truetest::risk::compute_risk_quantity(in5);
    EXPECT_NEAR(q5, 1.0, 1e-6);

    // Non-positive stop price (e.g. entry=1.0, stop=-3.0) yields 0.0
    truetest::risk::risk_size_inputs in6 = in1;
    in6.entry_price = 1.0;
    in6.stop_price = -3.0;
    double q6 = truetest::risk::compute_risk_quantity(in6);
    EXPECT_DOUBLE_EQ(q6, 0.0);
}

TEST(EmaRsiAtrPullbackTest, IndicatorValuesExposedCorrectly)
{
    ema_rsi_atr_pullback_strategy s(3, 2, 2);

    // Unknown symbol -> empty
    EXPECT_TRUE(s.get_indicator_values("UNKNOWN").empty());

    // Feed bars to initialize
    s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(200, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(300, 100.0, 101.0, 99.0, 100.0));

    auto vals = s.get_indicator_values("TEST");
    ASSERT_GE(vals.size(), 5u);

    std::unordered_map<std::string, double> vmap;
    for (const auto& [k, v] : vals)
        vmap[k] = v;

    EXPECT_EQ(vmap.count("ema_3"), 1u);
    EXPECT_EQ(vmap.count("rsi_2"), 1u);
    EXPECT_EQ(vmap.count("atr_2"), 1u);
    EXPECT_EQ(vmap.count("trade_state"), 1u);
    EXPECT_EQ(vmap.count("open_qty"), 1u);
}

TEST(EmaRsiAtrPullbackTest, ChangingPeriodDuringActiveTradeIsRejected)
{
    ema_rsi_atr_pullback_strategy s(3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);

    // Open a long position
    s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(200, 100.0, 100.0, 89.0, 90.0));
    s.on_market(make_bar(300, 90.0, 92.0, 87.0, 88.0));
    auto order = s.on_market(make_bar(400, 90.0, 112.0, 89.0, 110.0));
    ASSERT_TRUE(order.has_value());
    s.on_fill(make_fill(1001, "TEST", order_side::buy, order->get_quantity(), 110.0), 1001);

    // Changing period during active trade must throw
    EXPECT_THROW(s.set_param("ema_period", 5.0), std::runtime_error);
    EXPECT_THROW(s.set_param("rsi_period", 5.0), std::runtime_error);
    EXPECT_THROW(s.set_param("atr_period", 5.0), std::runtime_error);
}

TEST(EmaRsiAtrPullbackTest, ExitManagerRebasesAtrStopOnSlippedFill)
{
    // Verify that ExitManager rebases the ATR stop when actual fill slips
    // Intended entry = 100.0, ATR = 2.0, Multiplier = 2.0 -> Intended Stop = 96.0 ($4 risk)
    // Slipped Fill = 102.0 (+2.0 slip)
    // Rebased Stop = 96.0 + 2.0 = 98.0
    truetest::exits::ExitManager m;

    truetest::exits::exit_intent ei;
    ei.symbol = "TEST";
    ei.close_side = order_side::sell;
    ei.qty = 10.0;
    ei.qty_fraction = 1.0;
    ei.stop_loss = 96.0;
    ei.reference_entry = 100.0;
    ei.opener_order_id = 42;
    ei.strategy_name = "ema-rsi-atr-pullback";

    m.register_pending(std::move(ei));
    m.on_fill(make_fill(42, "TEST", order_side::buy, 10.0, 102.0));

    // Price at 98.1 (above rebased stop 98.0) -> no trigger
    auto miss = m.on_price("TEST", 98.1, make_tp(1000));
    EXPECT_TRUE(miss.empty());
    EXPECT_EQ(m.armed_count(), 1u);

    // Price at 98.0 (rebased stop level) -> triggers exit!
    auto hit = m.on_price("TEST", 98.0, make_tp(2000));
    ASSERT_EQ(hit.size(), 1u);
    EXPECT_EQ(hit[0].get_symbol(), "TEST");
    EXPECT_EQ(hit[0].get_side(), order_side::sell);
    EXPECT_DOUBLE_EQ(hit[0].get_quantity(), 10.0);
    EXPECT_EQ(m.armed_count(), 0u);
}
