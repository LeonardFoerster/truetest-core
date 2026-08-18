// Pins the look-ahead fix: a strategy that emits a market order on bar N must
// fill at bar N+1's open (not bar N's close) when execution_bar_delay=1,
// and fill at bar N's close when execution_bar_delay=0 (opt-out).
// Also pins LIMIT@close vs MARKET under a gap (ma-crossover/sma default path).

#include <gtest/gtest.h>
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "data/date_parse.h"
#include "orderbook/orderbook.h"
#include "orderbook/fill_model.h"
#include "market_maker/market_maker.h"
#include "strategy/ma_crossover_strategy.h"
#include "strategy/strategy_interface.h"

#include <chrono>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct SilenceCout {
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceCout() : sink(), orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~SilenceCout() { std::cout.rdbuf(orig); }
};

// Emits a single market buy on its 4th on_market call (0-indexed bar 3).
// Uses bar 3's close as intended price so slippage attribution lines up.
class OneShotBuyStrategy : public IStrategy
{
    int call_count_ = 0;
    bool fired_ = false;
public:
    int trigger_bar_index() const { return 3; }

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        if (call_count_++ == trigger_bar_index() && !fired_)
        {
            fired_ = true;
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::buy,
                               1.0, mkt.get_close());
        }
        return std::nullopt;
    }
    void set_position_open(const std::string&, bool) override {}
};

// Same timing as OneShotBuyStrategy but LIMIT@close — models legacy
// sma / ma-crossover fill_style=1. Gap-up next open must not reprice the limit.
class OneShotLimitBuyStrategy : public IStrategy
{
    int call_count_ = 0;
    bool fired_ = false;
public:
    int trigger_bar_index() const { return 3; }

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        if (call_count_++ == trigger_bar_index() && !fired_)
        {
            fired_ = true;
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::limit, order_side::buy,
                               1.0, mkt.get_close());
        }
        return std::nullopt;
    }
    void set_position_open(const std::string&, bool) override {}
};

class FirstSymbolBuyStrategy : public IStrategy
{
    bool fired_ = false;
public:
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        if (!fired_ && mkt.get_symbol() == "A")
        {
            fired_ = true;
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::buy,
                               1.0, mkt.get_close());
        }
        return std::nullopt;
    }
};

class EquityProbeStrategy : public IStrategy
{
    double current_equity_ = -1.0;
    bool fired_ = false;
public:
    std::vector<double> observed;

    void set_account_equity(double equity) override
    {
        current_equity_ = equity;
    }

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        observed.push_back(current_equity_);
        if (!fired_)
        {
            fired_ = true;
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::buy,
                               1.0, mkt.get_close());
        }
        return std::nullopt;
    }
};

class FirstMatchingBarBuyStrategy : public IStrategy
{
    bool fired_ = false;
public:
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        if (fired_ || mkt.get_symbol() != "A")
            return std::nullopt;
        fired_ = true;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy,
                           0.01, mkt.get_close());
    }
};

class NoOrderStrategy : public IStrategy
{
public:
    std::optional<order_event> on_market(const market_event&) override
    {
        return std::nullopt;
    }
};

std::string delayed_strategy_name(std::size_t index)
{
    return "delayed_" + std::to_string(index);
}

// Bars 0..2 and 4..6 are flat at ~100; bar 3 closes at 100; bar 4 opens
// at 200 so a "next-bar open" fill is distinguishable from a "same-bar close"
// fill by orders of magnitude.
std::shared_ptr<data_handler> make_gap_bars()
{
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 3; ++i)
        dh->load_into_queue("2024-01-01", "TEST", 100.0, 101.0, 99.0, 100.0, 1000);
    dh->load_into_queue("2024-01-01", "TEST", 100.0, 101.0, 99.0, 100.0, 1000);
    dh->load_into_queue("2024-01-01", "TEST", 200.0, 201.0, 199.0, 200.0, 1000);
    for (int i = 0; i < 3; ++i)
        dh->load_into_queue("2024-01-01", "TEST", 200.0, 201.0, 199.0, 200.0, 1000);
    return dh;
}

}

TEST(EngineLookahead, Default_OrderFillsAtNextBarOpen)
{
    SilenceCout quiet;
    auto dh = make_gap_bars();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<OneShotBuyStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.seed = 1;                      // fill/MM RNG; bar clock uses series ts
    cfg.initial_balance = 100000.0;
    // Isolate bar-delay fill pricing from platform DefaultExitPolicy.
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    ASSERT_EQ(cfg.execution_bar_delay, 1u) << "default must be 1 bar of delay";

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    auto report = eng.get_analytics().generate_report();
    ASSERT_EQ(report.trades.size(), 1u);
    const auto& t = report.trades.front();

    // Fill must have happened strictly AFTER bar 3's timestamp. Same-day CSV
    // dates share midnight ts; engine steps +1ms for non-monotonic ties.
    auto day0 = *tt::date_parse::parse("2024-01-01");
    auto trigger_ts = day0 + std::chrono::milliseconds(strat->trigger_bar_index());
    EXPECT_GT(t.timestamp, trigger_ts)
        << "order emitted on bar 3 must not fill within bar 3 under default bar-delay";

    // Fill price must reflect bar 4's open (~200), not bar 3's close
    // (~100): the engine re-seeds the synthetic book at the open before
    // draining pending orders, and fills record resting book prices.
    EXPECT_GT(t.fill_price, 150.0)
        << "fill price should track bar 4's open (~200), not bar 3's close (~100)";
}

TEST(EngineLookahead, Disabled_OrderFillsSameBar)
{
    SilenceCout quiet;
    auto dh = make_gap_bars();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<OneShotBuyStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.seed = 1;
    cfg.initial_balance = 100000.0;
    cfg.execution_bar_delay = 0;       // opt out: restore legacy behavior
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    auto report = eng.get_analytics().generate_report();
    ASSERT_EQ(report.trades.size(), 1u);
    const auto& t = report.trades.front();

    // With delay disabled the fill should carry bar 3's series timestamp
    // (same-day bars → midnight + index ms) and bar 3 close (~100).
    auto day0 = *tt::date_parse::parse("2024-01-01");
    auto trigger_ts = day0 + std::chrono::milliseconds(strat->trigger_bar_index());
    EXPECT_EQ(t.timestamp, trigger_ts);
    EXPECT_LT(t.fill_price, 150.0);
}

TEST(EngineLookahead, DelayTwoWaitsForSecondFutureSameSymbolBar)
{
    SilenceCout quiet;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 4; ++i)
        dh->load_into_queue("2024-01-01", "TEST", 100.0, 101.0, 99.0,
                            100.0, 1000);
    dh->load_into_queue("2024-01-01", "TEST", 200.0, 201.0, 199.0,
                        200.0, 1000);
    dh->load_into_queue("2024-01-01", "TEST", 300.0, 301.0, 299.0,
                        300.0, 1000);

    engine_config cfg;
    cfg.seed = 1;
    cfg.initial_balance = 100000.0;
    cfg.execution_bar_delay = 2;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;

    engine eng(dh, nullptr, std::make_shared<OneShotBuyStrategy>(),
               std::move(cfg));
    eng.run();

    const auto report = eng.get_analytics().generate_report();
    ASSERT_EQ(report.trades.size(), 1u);
    EXPECT_GT(report.trades.front().fill_price, 250.0)
        << "delay=2 must fill at the second future bar, not merely +1ns";
}

TEST(EngineLookahead, MultiSymbolDelayWaitsForNextSameSymbolOpen)
{
    SilenceCout quiet;
    auto dh = std::make_shared<data_handler>();
    const auto base = std::chrono::system_clock::time_point{
        std::chrono::seconds{1'700'000'000}};
    auto add = [&](std::chrono::seconds offset, const char* symbol, double px) {
        Bar bar;
        bar.ts = base + offset;
        bar.symbol = symbol;
        bar.open = bar.high = bar.low = bar.close = px;
        bar.volume = 1000;
        ASSERT_TRUE(dh->on_bar(bar));
    };
    add(std::chrono::seconds{0}, "A", 100.0);
    add(std::chrono::seconds{1}, "B", 1000.0);
    add(std::chrono::seconds{2}, "A", 200.0);

    engine_config cfg;
    cfg.seed = 1;
    cfg.initial_balance = 100000.0;
    cfg.execution_bar_delay = 1;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;

    engine eng(dh, nullptr, std::make_shared<FirstSymbolBuyStrategy>(),
               std::move(cfg));
    eng.run();

    const auto report = eng.get_analytics().generate_report();
    ASSERT_EQ(report.trades.size(), 1u);
    EXPECT_GT(report.trades.front().fill_price, 150.0)
        << "symbol B must not release a delayed order for symbol A";
}

TEST(EngineLookahead, DelayedSchedulerStableAtBoundedBacklogSizes)
{
    SilenceCout quiet;
    for (const std::size_t pending_count : {0U, 1U, 100U, 1000U})
    {
        SCOPED_TRACE(::testing::Message()
                     << "pending_count=" << pending_count);

        auto dh = std::make_shared<data_handler>();
        const auto base = std::chrono::system_clock::time_point{
            std::chrono::seconds{1'700'100'000}};
        auto add = [&](std::chrono::seconds offset,
                       const char* symbol, double px) {
            Bar bar;
            bar.ts = base + offset;
            bar.symbol = symbol;
            bar.open = bar.high = bar.low = bar.close = px;
            bar.volume = 1000;
            ASSERT_TRUE(dh->on_bar(bar));
        };
        add(std::chrono::seconds{0}, "A", 100.0);
        add(std::chrono::seconds{1}, "B", 1000.0);
        add(std::chrono::seconds{2}, "A", 200.0);

        std::shared_ptr<IStrategy> primary =
            std::make_shared<NoOrderStrategy>();
        if (pending_count > 0)
            primary = std::make_shared<FirstMatchingBarBuyStrategy>();

        engine_config cfg;
        cfg.seed = 42;
        cfg.show_progress = false;
        cfg.initial_balance = 1'000'000.0;
        cfg.execution_bar_delay = 1;
        cfg.fill_model = std::make_shared<PerfectFillModel>();
        cfg.risk.max_open_orders = static_cast<int>(
            pending_count > 0 ? pending_count : 1);
        cfg.exit_defaults.mode =
            truetest::exits::exit_policy_mode::strategy_only;

        engine eng(dh, nullptr, primary, std::move(cfg));
        if (pending_count > 0)
        {
            eng.set_primary_strategy_name(delayed_strategy_name(0));
            for (std::size_t i = 1; i < pending_count; ++i)
            {
                eng.add_strategy(
                    std::make_shared<FirstMatchingBarBuyStrategy>(),
                    delayed_strategy_name(i));
            }
        }

        eng.run();

        const auto report = eng.get_analytics().generate_report();
        EXPECT_EQ(report.total_orders, pending_count);
        EXPECT_EQ(report.total_fills, pending_count);
        ASSERT_EQ(report.trades.size(), pending_count);
        for (std::size_t i = 0; i < report.trades.size(); ++i)
        {
            const auto& trade = report.trades[i];
            EXPECT_EQ(trade.symbol, "A");
            EXPECT_EQ(trade.strategy_name, delayed_strategy_name(i))
                << "due orders must submit in insertion order";
            EXPECT_EQ(trade.timestamp, base + std::chrono::seconds{2})
                << "symbol B must not release symbol A's delayed orders";
            EXPECT_GT(trade.fill_price, 150.0);
            EXPECT_LT(trade.fill_price, 300.0);
        }
    }
}

TEST(EngineLookahead, FinalBarSignalHasNoSyntheticEosFill)
{
    SilenceCout quiet;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 4; ++i)
        dh->load_into_queue("2024-01-01", "TEST", 100.0, 101.0, 99.0,
                            100.0, 1000);

    engine_config cfg;
    cfg.seed = 1;
    cfg.initial_balance = 100000.0;
    cfg.execution_bar_delay = 1;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;

    engine eng(dh, nullptr, std::make_shared<OneShotBuyStrategy>(),
               std::move(cfg));
    eng.run();

    const auto report = eng.get_analytics().generate_report();
    EXPECT_EQ(report.total_orders, 0u)
        << "never-submitted delayed candidates are not venue orders";
    EXPECT_EQ(report.total_fills, 0u);
    EXPECT_TRUE(report.trades.empty());
    EXPECT_EQ(eng.get_order_tracker().active_count(), 0u)
        << "EOF expiration must release the pending lifecycle slot";
}

TEST(EngineLookahead, StrategySizingReceivesCurrentMarkedAccountEquity)
{
    SilenceCout quiet;
    auto dh = std::make_shared<data_handler>();
    dh->load_into_queue("2024-01-01", "TEST", 100.0, 101.0, 99.0,
                        100.0, 1000);
    dh->load_into_queue("2024-01-02", "TEST", 80.0, 81.0, 79.0,
                        80.0, 1000);

    auto strategy = std::make_shared<EquityProbeStrategy>();
    engine_config cfg;
    cfg.seed = 1;
    cfg.initial_balance = 10000.0;
    cfg.execution_bar_delay = 0;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;

    engine eng(dh, nullptr, strategy, std::move(cfg));
    eng.run();

    ASSERT_EQ(strategy->observed.size(), 2u);
    EXPECT_NEAR(strategy->observed[0], 10000.0, 1e-9);
    EXPECT_LT(strategy->observed[1], strategy->observed[0])
        << "the second sizing decision must see the marked loss, not static "
           "configured equity";
}

TEST(EngineLookahead, StrategyCallbackAfterIntrabarExitReceivesPostExitPortfolioCash)
{
    SilenceCout quiet;
    auto dh = std::make_shared<data_handler>();
    dh->load_into_queue("2024-01-01", "TEST", 100.0, 101.0, 99.0,
                        100.0, 1000);
    // The delayed entry fills at this bar's open. Its 1% stop is crossed by
    // the low, while the close deliberately marks the pre-exit position far
    // above the stop fill so stale Analytics equity is distinguishable.
    dh->load_into_queue("2024-01-02", "TEST", 100.0, 121.0, 90.0,
                        120.0, 1000);

    auto strategy = std::make_shared<EquityProbeStrategy>();
    engine_config cfg;
    constexpr double initial_balance = 10000.0;
    cfg.seed = 1;
    cfg.initial_balance = initial_balance;
    cfg.execution_bar_delay = 1;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::engine_only;
    cfg.exit_defaults.sl_pct = 0.01;
    cfg.exit_defaults.tp_pct = 0.0;

    engine eng(dh, nullptr, strategy, std::move(cfg));
    eng.run();

    const auto report = eng.get_analytics().generate_report();
    ASSERT_EQ(strategy->observed.size(), 2u);
    ASSERT_EQ(report.trades.size(), 2u);
    const auto& entry = report.trades[0];
    const auto& exit = report.trades[1];
    ASSERT_EQ(entry.side, order_side::buy);
    ASSERT_EQ(exit.side, order_side::sell);
    ASSERT_DOUBLE_EQ(entry.quantity, exit.quantity);

    double expected_post_exit_cash = initial_balance;
    expected_post_exit_cash -=
        entry.quantity * entry.fill_price + entry.commission;
    expected_post_exit_cash +=
        exit.quantity * exit.fill_price - exit.commission;

    // Portfolio is flat before the second callback, so marked account equity
    // must be exactly its post-exit cash ledger.
    EXPECT_DOUBLE_EQ(strategy->observed[1], expected_post_exit_cash);
    EXPECT_NEAR(report.final_equity, expected_post_exit_cash, 1e-9);

    double stale_pre_exit_equity = initial_balance;
    stale_pre_exit_equity -=
        entry.quantity * entry.fill_price + entry.commission;
    stale_pre_exit_equity += entry.quantity * 120.0;
    EXPECT_GT(stale_pre_exit_equity - expected_post_exit_cash, 1.0)
        << "fixture must distinguish the close-marked pre-exit value";
    EXPECT_GT(stale_pre_exit_equity - strategy->observed[1], 1.0)
        << "strategy callback must not receive stale pre-exit Analytics equity";
}

// LIMIT priced at bar-N close does NOT re-anchor to bar N+1 open. On a gap-up
// the limit rests below the open and never fills if the series stays elevated.
TEST(EngineLookahead, LimitAtClose_MissesGapUpOpen)
{
    SilenceCout quiet;
    auto dh = make_gap_bars();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<OneShotLimitBuyStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.seed = 1;
    cfg.initial_balance = 100000.0;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    ASSERT_EQ(cfg.execution_bar_delay, 1u);

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    auto report = eng.get_analytics().generate_report();
    EXPECT_TRUE(report.trades.empty())
        << "LIMIT@100 must not fill when next bar opens at ~200 and stays there";
}

// ma-crossover default (market + delay=1): golden-cross signal on close fills
// near the next bar's open, not the signal close — classical bar convention.
TEST(EngineLookahead, MaCrossover_DefaultMarketFillsNextOpen)
{
    SilenceCout quiet;
    // Falling then spike so fast=2/slow=3 golden-crosses on the spike bar;
    // following bar gaps up so open-fill is distinguishable from close-fill.
    auto dh = std::make_shared<data_handler>();
    // 100,90,80 → first ready bar seeds prev_fast_above=false (fast 85 < slow 90)
    dh->load_into_queue("2024-01-01", "TEST", 100.0, 101.0, 99.0, 100.0, 1000);
    dh->load_into_queue("2024-01-01", "TEST", 90.0, 91.0, 89.0, 90.0, 1000);
    dh->load_into_queue("2024-01-01", "TEST", 80.0, 81.0, 79.0, 80.0, 1000);
    // Golden cross: close 120 (fast 100 > slow 96.67). Signal bar.
    dh->load_into_queue("2024-01-01", "TEST", 80.0, 121.0, 79.0, 120.0, 1000);
    // Next open 200 — market order drained here under default bar delay.
    dh->load_into_queue("2024-01-01", "TEST", 200.0, 201.0, 199.0, 200.0, 1000);
    dh->load_into_queue("2024-01-01", "TEST", 200.0, 201.0, 199.0, 200.0, 1000);

    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<ma_crossover_strategy>(2, 3);

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.seed = 1;
    cfg.initial_balance = 1000000.0; // qty 100 * ~200 needs headroom
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    ASSERT_EQ(cfg.execution_bar_delay, 1u);

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    auto report = eng.get_analytics().generate_report();
    ASSERT_FALSE(report.trades.empty()) << "market default must fill on next open";
    const auto& t = report.trades.front();
    EXPECT_EQ(t.side, order_side::buy);
    EXPECT_GT(t.fill_price, 150.0)
        << "default ma-crossover market must track next open (~200), not signal close (~120)";
}
