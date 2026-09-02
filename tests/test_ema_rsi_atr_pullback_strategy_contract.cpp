#include <gtest/gtest.h>

#include "reference/ema_rsi_atr_pullback_reference.h"
#include "data/data_handler.h"
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "execution/portfolio.h"
#include "strategy/ema_rsi_atr_pullback/ema_rsi_atr_pullback_strategy.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

auto make_timepoint(int64_t milliseconds)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(milliseconds));
}

market_event make_bar(int64_t milliseconds, const std::string& symbol,
                      double open, double high, double low, double close)
{
    return market_event(make_timepoint(milliseconds), symbol, open, high, low, close, 100);
}

market_event make_bar(int64_t milliseconds, double open, double high, double low, double close)
{
    return make_bar(milliseconds, "TEST", open, high, low, close);
}

fill_event make_fill(uint64_t order_id, const std::string& symbol,
                     order_side side, double quantity, double price)
{
    return fill_event(make_timepoint(0), symbol, order_id, side, quantity, price,
                      /*commission=*/0.0, /*remaining=*/0.0, /*fill_id=*/1);
}

std::unordered_map<std::string, double> indicator_map(
    const ema_rsi_atr_pullback_strategy& strategy, const std::string& symbol)
{
    std::unordered_map<std::string, double> values;
    for (const auto& [name, value] : strategy.get_indicator_values(symbol))
        values.emplace(name, value);
    return values;
}

class CountingEmaRsiAtrPullbackStrategy final : public ema_rsi_atr_pullback_strategy
{
public:
    using ema_rsi_atr_pullback_strategy::ema_rsi_atr_pullback_strategy;

    std::optional<order_event> on_market(const market_event& market) override
    {
        auto order = ema_rsi_atr_pullback_strategy::on_market(market);
        if (order.has_value())
            ++orders_emitted;
        return order;
    }

    int orders_emitted = 0;
};

} // namespace

TEST(EmaRsiAtrPullbackContractTest, ExactBoundarySemantics)
{
    // With RSI(2), changes -3 then +2 produce RSI[t-1] = 40. The
    // following flat bar is still RSI = 40 and must not enter because the
    // current-value comparison is strict; the later fresh recross may enter.
    ema_rsi_atr_pullback_strategy long_strategy(2, 2, 1);
    long_strategy.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    long_strategy.on_market(make_bar(200, 100.0, 101.0, 96.0, 97.0));
    long_strategy.on_market(make_bar(300, 97.0, 100.0, 96.0, 99.0));
    EXPECT_EQ(long_strategy.on_market(make_bar(400, 99.0, 100.0, 98.0, 99.0)), std::nullopt);
    auto long_order = long_strategy.on_market(make_bar(500, 99.0, 101.0, 98.0, 100.0));
    ASSERT_TRUE(long_order.has_value());
    EXPECT_EQ(long_order->get_side(), order_side::buy);

    // The short side has the symmetric previous-value-inclusive/current-value
    // strict policy at RSI = 60.
    ema_rsi_atr_pullback_strategy short_strategy(2, 2, 1);
    short_strategy.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    short_strategy.on_market(make_bar(200, 100.0, 104.0, 99.0, 103.0));
    short_strategy.on_market(make_bar(300, 103.0, 104.0, 100.0, 101.0));
    EXPECT_EQ(short_strategy.on_market(make_bar(400, 101.0, 102.0, 100.0, 101.0)), std::nullopt);
    auto short_order = short_strategy.on_market(make_bar(500, 101.0, 102.0, 99.0, 100.0));
    ASSERT_TRUE(short_order.has_value());
    EXPECT_EQ(short_order->get_side(), order_side::sell);

    // close == EMA is neutral even if the RSI recross itself is valid.
    ema_rsi_atr_pullback_strategy equal_ema_strategy(2, 2, 1);
    equal_ema_strategy.on_market(make_bar(100, 101.0, 102.0, 100.0, 101.0));
    equal_ema_strategy.on_market(make_bar(200, 101.0, 104.0, 100.0, 103.0));
    equal_ema_strategy.on_market(make_bar(300, 103.0, 104.0, 98.0, 99.0));
    auto neutral_order = equal_ema_strategy.on_market(make_bar(400, 99.0, 101.0, 98.0, 100.0));
    EXPECT_EQ(neutral_order, std::nullopt);
    const auto values = indicator_map(equal_ema_strategy, "TEST");
    ASSERT_EQ(values.count("ema_2"), 1u);
    EXPECT_DOUBLE_EQ(values.at("ema_2"), 100.0);
}

TEST(EmaRsiAtrPullbackContractTest, IndependentReferenceMatchesFlatStrategySignals)
{
    using ema_rsi_atr_reference::bar;
    using ema_rsi_atr_reference::evaluator;
    using ema_rsi_atr_reference::signal;

    constexpr std::string_view symbol = "ORACLE";
    ema_rsi_atr_pullback_strategy strategy(3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);
    evaluator reference(3, 2, 2, 40.0, 60.0);
    const std::vector<bar> bars{
        {100.0, 101.0, 99.0, 100.0},
        {100.0, 101.0, 89.0, 90.0},
        {90.0, 92.0, 87.0, 88.0},
        {88.0, 112.0, 87.0, 110.0},
        {110.0, 113.0, 109.0, 112.0},
        {112.0, 113.0, 89.0, 90.0},
        {90.0, 122.0, 89.0, 120.0},
        {120.0, 126.0, 99.0, 100.0},
    };

    for (std::size_t i = 0; i < bars.size(); ++i)
    {
        const auto& input = bars[i];
        const auto expected = reference.on_bar(input);
        const auto state_before = strategy.get_trade_state(std::string(symbol));
        auto actual = strategy.on_market(make_bar(static_cast<int64_t>((i + 1) * 100),
                                                  std::string(symbol), input.open, input.high,
                                                  input.low, input.close));
        const auto state_after = strategy.get_trade_state(std::string(symbol));

        SCOPED_TRACE(expected.evidence());
        SCOPED_TRACE("state_before=" + std::to_string(static_cast<int>(state_before)) +
                     " decision=" + (actual.has_value() ? "order" : "none") +
                     " state_after=" + std::to_string(static_cast<int>(state_after)));

        const auto values = indicator_map(strategy, std::string(symbol));
        if (expected.ema.has_value())
        {
            ASSERT_EQ(values.count("ema_3"), 1u);
            EXPECT_NEAR(values.at("ema_3"), *expected.ema, 1e-12);
        }
        if (expected.rsi_current.has_value())
        {
            ASSERT_EQ(values.count("rsi_2"), 1u);
            EXPECT_NEAR(values.at("rsi_2"), *expected.rsi_current, 1e-12);
        }
        if (expected.atr.has_value())
        {
            ASSERT_EQ(values.count("atr_2"), 1u);
            EXPECT_NEAR(values.at("atr_2"), *expected.atr, 1e-12);
        }

        const bool expects_order = expected.expected != signal::none;
        EXPECT_EQ(actual.has_value(), expects_order);
        if (!expects_order)
        {
            EXPECT_EQ(state_after, ema_rsi_atr_pullback_strategy::trade_state::flat);
            continue;
        }

        ASSERT_TRUE(actual.has_value());
        EXPECT_EQ(state_before, ema_rsi_atr_pullback_strategy::trade_state::flat);
        EXPECT_EQ(actual->get_side(), expected.expected == signal::buy ? order_side::buy : order_side::sell);
        EXPECT_EQ(state_after, expected.expected == signal::buy
            ? ema_rsi_atr_pullback_strategy::trade_state::entry_pending_long
            : ema_rsi_atr_pullback_strategy::trade_state::entry_pending_short);

        auto intents = strategy.take_pending_exit_intents();
        ASSERT_EQ(intents.size(), 1u);
        ASSERT_TRUE(intents.front().stop_loss.has_value());
        const double expected_stop = expected.expected == signal::buy
            ? input.close - *expected.atr * 2.0
            : input.close + *expected.atr * 2.0;
        EXPECT_NEAR(*intents.front().stop_loss, expected_stop, 1e-12);

        // This is the engine's terminal non-fill notification. Keep the
        // indicator stream intact so the next order must be a fresh recross.
        strategy.set_position_open(std::string(symbol), false);
        EXPECT_EQ(strategy.get_trade_state(std::string(symbol)),
                  ema_rsi_atr_pullback_strategy::trade_state::flat);
    }
}

TEST(EmaRsiAtrPullbackContractTest, RejectedEntryConsumesRecrossUntilANewCross)
{
    ema_rsi_atr_pullback_strategy s(3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);
    s.set_param("allow_short", 0.0);

    s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(200, 100.0, 100.0, 89.0, 90.0));
    s.on_market(make_bar(300, 90.0, 92.0, 87.0, 88.0));
    auto rejected_entry = s.on_market(make_bar(400, 88.0, 112.0, 87.0, 110.0));
    ASSERT_TRUE(rejected_entry.has_value());
    EXPECT_EQ(s.take_pending_exit_intents().size(), 1u); // Engine drains before rejection feedback.

    // Mirrors OrderIntentProcessor's terminal non-fill notification. It
    // unlocks the entry state but deliberately does not restore rsi[t-1].
    s.set_position_open("TEST", false);
    EXPECT_EQ(s.get_trade_state("TEST"), ema_rsi_atr_pullback_strategy::trade_state::flat);
    EXPECT_EQ(s.on_market(make_bar(500, 110.0, 113.0, 109.0, 112.0)), std::nullopt)
        << "a rejection must not replay its already-consumed recross";

    // Drop below the threshold, then make a fresh qualifying recross.
    EXPECT_EQ(s.on_market(make_bar(600, 112.0, 113.0, 89.0, 90.0)), std::nullopt);
    auto fresh_entry = s.on_market(make_bar(700, 90.0, 122.0, 89.0, 120.0));
    ASSERT_TRUE(fresh_entry.has_value());
    EXPECT_EQ(fresh_entry->get_side(), order_side::buy);
}

TEST(EmaRsiAtrPullbackContractTest, DelayedEngineRejectionReturnsActualStrategyToFlat)
{
    auto data = std::make_shared<data_handler>();
    data->load_into_queue("2024-01-01", "TEST", 100.0, 101.0, 99.0, 100.0, 1000);
    data->load_into_queue("2024-01-01", "TEST", 100.0, 100.0, 89.0, 90.0, 1000);
    data->load_into_queue("2024-01-01", "TEST", 90.0, 92.0, 87.0, 88.0, 1000);
    data->load_into_queue("2024-01-01", "TEST", 88.0, 112.0, 87.0, 110.0, 1000);
    data->load_into_queue("2024-01-01", "TEST", 110.0, 113.0, 109.0, 112.0, 1000);

    auto strategy = std::make_shared<CountingEmaRsiAtrPullbackStrategy>(
        3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);
    engine_config config;
    config.mode = engine_mode::backtest;
    config.show_progress = false;
    config.initial_balance = 10000.0;
    config.execution_bar_delay = 1;
    config.risk.max_drawdown = 0.0; // Reject every order through the delayed path.
    config.risk_soft_portfolio_limits = true;

    engine eng(data, nullptr, strategy, std::move(config));
    eng.run();

    EXPECT_EQ(strategy->orders_emitted, 1);
    EXPECT_EQ(strategy->get_trade_state("TEST"), ema_rsi_atr_pullback_strategy::trade_state::flat);
    EXPECT_TRUE(strategy->take_pending_exit_intents().empty());
}

TEST(EmaRsiAtrPullbackContractTest, InterleavedSymbolsAndResetDoNotShareIndicatorOrTradeState)
{
    ema_rsi_atr_pullback_strategy s(3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);
    const auto feed = [&s](int64_t timestamp, const std::string& symbol,
                           double open, double high, double low, double close) {
        return s.on_market(make_bar(timestamp, symbol, open, high, low, close));
    };

    for (const auto& symbol : {std::string("SYM_A"), std::string("SYM_B")})
        EXPECT_EQ(feed(100, symbol, 100.0, 101.0, 99.0, 100.0), std::nullopt);
    for (const auto& symbol : {std::string("SYM_A"), std::string("SYM_B")})
        EXPECT_EQ(feed(200, symbol, 100.0, 100.0, 89.0, 90.0), std::nullopt);
    for (const auto& symbol : {std::string("SYM_A"), std::string("SYM_B")})
        EXPECT_EQ(feed(300, symbol, 90.0, 92.0, 87.0, 88.0), std::nullopt);

    auto a_order = feed(400, "SYM_A", 88.0, 112.0, 87.0, 110.0);
    auto b_order = feed(400, "SYM_B", 88.0, 112.0, 87.0, 110.0);
    ASSERT_TRUE(a_order.has_value());
    ASSERT_TRUE(b_order.has_value());
    EXPECT_EQ(s.get_trade_state("SYM_A"), ema_rsi_atr_pullback_strategy::trade_state::entry_pending_long);
    EXPECT_EQ(s.get_trade_state("SYM_B"), ema_rsi_atr_pullback_strategy::trade_state::entry_pending_long);
    EXPECT_EQ(s.take_pending_exit_intents().size(), 2u);

    s.reset(99);
    EXPECT_EQ(s.get_trade_state("SYM_A"), ema_rsi_atr_pullback_strategy::trade_state::flat);
    EXPECT_EQ(s.get_trade_state("SYM_B"), ema_rsi_atr_pullback_strategy::trade_state::flat);
    EXPECT_TRUE(s.take_pending_exit_intents().empty());

    // B must be able to warm and signal again without reviving A's prior RSI
    // or pending state.
    feed(500, "SYM_B", 100.0, 101.0, 99.0, 100.0);
    feed(600, "SYM_B", 100.0, 100.0, 89.0, 90.0);
    feed(700, "SYM_B", 90.0, 92.0, 87.0, 88.0);
    EXPECT_TRUE(feed(800, "SYM_B", 88.0, 112.0, 87.0, 110.0).has_value());
    EXPECT_EQ(s.get_trade_state("SYM_A"), ema_rsi_atr_pullback_strategy::trade_state::flat);
}

TEST(EmaRsiAtrPullbackContractTest, OppositeSignalClosesLongWithoutReversingAndPartialCloseResyncs)
{
    ema_rsi_atr_pullback_strategy s(3, 2, 2, 0.01, 2.0, 10000.0, 40.0, 60.0);
    portfolio portfolio_state(10000.0);
    s.on_market(make_bar(100, 100.0, 101.0, 99.0, 100.0));
    s.on_market(make_bar(200, 100.0, 100.0, 89.0, 90.0));
    s.on_market(make_bar(300, 90.0, 92.0, 87.0, 88.0));
    auto entry = s.on_market(make_bar(400, 88.0, 112.0, 87.0, 110.0));
    ASSERT_TRUE(entry.has_value());
    const double quantity = entry->get_quantity();
    (void)s.take_pending_exit_intents();
    const auto entry_fill = make_fill(7001, "TEST", order_side::buy, quantity, 110.0);
    s.on_fill(entry_fill, 7001);
    portfolio_state.on_fill(entry_fill, 7001, "ema-rsi-atr-pullback");
    EXPECT_NEAR(portfolio_state.get_positions().at("TEST").qty, s.get_open_qty("TEST"), 1e-6);

    // This bar satisfies the raw short predicate while a long is open. The
    // strategy may only emit the attributed long closer, never reverse.
    auto first_close = s.on_market(make_bar(500, 110.0, 111.0, 89.0, 90.0));
    ASSERT_TRUE(first_close.has_value());
    EXPECT_EQ(first_close->get_side(), order_side::sell);
    EXPECT_EQ(first_close->get_opener_order_id(), 7001u);
    EXPECT_EQ(first_close->get_quantity(), quantity);
    EXPECT_EQ(s.get_trade_state("TEST"), ema_rsi_atr_pullback_strategy::trade_state::exit_pending_long);

    const auto partial_close_fill = make_fill(7002, "TEST", order_side::sell, quantity * 0.4, 90.0);
    s.on_fill(partial_close_fill, 7001);
    portfolio_state.on_fill(partial_close_fill, 7001, "ema-rsi-atr-pullback");
    EXPECT_NEAR(s.get_open_qty("TEST"), quantity * 0.6, 1e-6);
    EXPECT_NEAR(portfolio_state.get_positions().at("TEST").qty, s.get_open_qty("TEST"), 1e-6);
    // FillProcessor makes this net-position callback after each fill. The
    // strategy must converge back to long_open for the remaining quantity.
    s.set_position_open("TEST", true);
    EXPECT_EQ(s.get_trade_state("TEST"), ema_rsi_atr_pullback_strategy::trade_state::long_open);

    auto second_close = s.on_market(make_bar(600, 90.0, 91.0, 79.0, 80.0));
    ASSERT_TRUE(second_close.has_value());
    EXPECT_EQ(second_close->get_side(), order_side::sell);
    EXPECT_EQ(second_close->get_opener_order_id(), 7001u);
    EXPECT_NEAR(second_close->get_quantity(), quantity * 0.6, 1e-6);
    EXPECT_EQ(s.get_trade_state("TEST"), ema_rsi_atr_pullback_strategy::trade_state::exit_pending_long);

    const auto final_close_fill = make_fill(7003, "TEST", order_side::sell, quantity * 0.6, 80.0);
    s.on_fill(final_close_fill, 7001);
    portfolio_state.on_fill(final_close_fill, 7001, "ema-rsi-atr-pullback");
    EXPECT_EQ(s.get_trade_state("TEST"), ema_rsi_atr_pullback_strategy::trade_state::flat);
    EXPECT_DOUBLE_EQ(s.get_open_qty("TEST"), 0.0);
    EXPECT_NEAR(portfolio_state.get_positions().at("TEST").qty, s.get_open_qty("TEST"), 1e-6);
}
