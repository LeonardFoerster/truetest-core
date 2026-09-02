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
#include "strategy/ma_crossover/ma_crossover_strategy.h"
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

class IntrabarEntryWithProtectionStrategy : public IStrategy
{
    bool fired_ = false;
    std::optional<truetest::exits::exit_intent> pending_exit_;
    order_type entry_type_;
    order_side entry_side_;
    double entry_price_;
    double trigger_price_;
    double quantity_;
    std::optional<double> stop_loss_;
    std::optional<double> take_profit_;

public:
    IntrabarEntryWithProtectionStrategy(
        order_type entry_type,
        order_side entry_side,
        double entry_price,
        double trigger_price,
        std::optional<double> stop_loss,
        std::optional<double> take_profit,
        double quantity = 1.0)
        : entry_type_(entry_type),
          entry_side_(entry_side),
          entry_price_(entry_price),
          trigger_price_(trigger_price),
          quantity_(quantity),
          stop_loss_(stop_loss),
          take_profit_(take_profit)
    {
    }

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        if (fired_)
            return std::nullopt;

        fired_ = true;
        truetest::exits::exit_intent intent;
        intent.symbol = mkt.get_symbol();
        intent.close_side = entry_side_ == order_side::buy
            ? order_side::sell : order_side::buy;
        intent.stop_loss = stop_loss_;
        intent.take_profit = take_profit_;
        intent.reference_entry = entry_type_ == order_type::limit
            ? entry_price_ : trigger_price_;
        intent.qty = quantity_;
        pending_exit_ = intent;

        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           entry_type_, entry_side_, quantity_, entry_price_,
                           time_in_force::gtc, trigger_price_);
    }

    std::optional<truetest::exits::exit_intent>
    take_pending_exit_intent() override
    {
        auto result = std::move(pending_exit_);
        pending_exit_.reset();
        return result;
    }
};

std::shared_ptr<data_handler> make_pathless_intrabar_series(
    double open, double high, double low, double close,
    std::int64_t ambiguous_volume = 1000)
{
    auto dh = std::make_shared<data_handler>();
    constexpr long long t0 = 1'704'067'200'000LL;
    dh->load_into_queue(std::to_string(t0), "TEST",
                        100.0, 101.0, 99.0, 100.0, 1000);
    dh->load_into_queue(std::to_string(t0 + 60'000), "TEST",
                        100.0, 101.0, 99.0, 100.0, 1000);
    dh->load_into_queue(std::to_string(t0 + 120'000), "TEST",
                        open, high, low, close, ambiguous_volume);
    dh->load_into_queue(std::to_string(t0 + 180'000), "TEST",
                        close, close + 1.0, close - 1.0, close, 1000);
    return dh;
}

engine_config make_pathless_intrabar_config()
{
    engine_config cfg;
    cfg.seed = 1;
    cfg.show_progress = false;
    cfg.initial_balance = 100'000.0;
    cfg.execution_bar_delay = 1;
    cfg.exit_defaults.mode =
        truetest::exits::exit_policy_mode::strategy_only;
    return cfg;
}

void expect_ambiguous_bar_rejected_without_economic_mutation(engine& eng)
{
    EXPECT_TRUE(eng.is_halted());
    const auto report = eng.get_analytics().generate_report();
    EXPECT_EQ(report.total_fills, 0u);
    EXPECT_TRUE(report.trades.empty());
    EXPECT_EQ(eng.get_portfolio().get_positions().count("TEST"), 0u);
    EXPECT_TRUE(eng.get_portfolio().get_lots().empty());
    EXPECT_DOUBLE_EQ(eng.get_portfolio().get_cash(), 100'000.0);
    EXPECT_EQ(eng.get_exit_manager().armed_count(), 0u);
    eng.get_order_tracker().for_each_order([](const order_ledger_entry& order) {
        EXPECT_DOUBLE_EQ(order.filled_qty, 0.0);
    });
}

void expect_unambiguous_round_trip(engine& eng,
                                   double min_exit_price,
                                   double max_exit_price)
{
    EXPECT_FALSE(eng.is_halted());
    const auto report = eng.get_analytics().generate_report();
    ASSERT_EQ(report.total_fills, 2u);
    ASSERT_EQ(report.trades.size(), 2u);
    EXPECT_GT(report.trades.back().fill_price, min_exit_price);
    EXPECT_LT(report.trades.back().fill_price, max_exit_price);
    const auto pos = eng.get_portfolio().get_positions().find("TEST");
    ASSERT_NE(pos, eng.get_portfolio().get_positions().end());
    EXPECT_DOUBLE_EQ(pos->second.qty, 0.0);
    EXPECT_TRUE(eng.get_portfolio().get_lots().empty());
    EXPECT_EQ(eng.get_exit_manager().armed_count(), 0u);
}

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

TEST(EngineLookahead,
     C01_PathlessBarDoesNotReusePreFillHighForNewTakeProfit)
{
    SilenceCout quiet;

    // The order is decided on bar 0, rests through bar 1, and is traversed at
    // 95 on bar 2.  A path O-H-L-C is compatible with bar 2:
    // H=110 prints before the buy entry at L=90.  Pathless OHLC therefore
    // cannot prove that the newly armed TP=105 was available after the fill.
    auto strategy = std::make_shared<IntrabarEntryWithProtectionStrategy>(
        order_type::limit, order_side::buy,
        /*entry_price=*/95.0, /*trigger_price=*/0.0,
        /*stop_loss=*/std::nullopt, /*take_profit=*/105.0);
    engine eng(make_pathless_intrabar_series(100.0, 110.0, 90.0, 100.0),
               nullptr, strategy, make_pathless_intrabar_config());
    eng.run();

    expect_ambiguous_bar_rejected_without_economic_mutation(eng);
}

TEST(EngineLookahead,
     C01_PathlessBarDoesNotReusePreFillLowForNewStopLoss)
{
    SilenceCout quiet;

    // O-L-H-C is compatible with this bar: L=90 prints before the buy-stop
    // entry triggers at H=110.  The newly armed SL=95 must not reuse that low.
    auto strategy = std::make_shared<IntrabarEntryWithProtectionStrategy>(
        order_type::stop, order_side::buy,
        /*entry_price=*/0.0, /*trigger_price=*/105.0,
        /*stop_loss=*/95.0, /*take_profit=*/std::nullopt);
    engine eng(make_pathless_intrabar_series(100.0, 110.0, 90.0, 107.0),
               nullptr, strategy, make_pathless_intrabar_config());
    eng.run();

    expect_ambiguous_bar_rejected_without_economic_mutation(eng);
}

TEST(EngineLookahead,
     C01_PathlessStopLimitDoesNotReusePreFillLowForNewStopLoss)
{
    SilenceCout quiet;

    // The stop-limit triggers at 105 and its 106 limit is marketable against
    // the trigger-anchored synthetic ask.  In a compatible O-L-H-C path the
    // low occurred before both trigger and fill, so it cannot fire the new SL.
    auto strategy = std::make_shared<IntrabarEntryWithProtectionStrategy>(
        order_type::stop_limit, order_side::buy,
        /*entry_price=*/106.0, /*trigger_price=*/105.0,
        /*stop_loss=*/95.0, /*take_profit=*/std::nullopt);
    engine eng(make_pathless_intrabar_series(100.0, 110.0, 90.0, 107.0),
               nullptr, strategy, make_pathless_intrabar_config());
    eng.run();

    expect_ambiguous_bar_rejected_without_economic_mutation(eng);
}

TEST(EngineLookahead,
     C01_PathlessShortDoesNotReusePreFillLowForNewTakeProfit)
{
    SilenceCout quiet;

    // O-L-H-C is compatible with this bar: L=90 prints before the resting
    // sell-limit entry at H=110.  The newly armed short TP=95 cannot reuse
    // that earlier low.  This mirrors the long limit/TP causality failure.
    auto strategy = std::make_shared<IntrabarEntryWithProtectionStrategy>(
        order_type::limit, order_side::sell,
        /*entry_price=*/105.0, /*trigger_price=*/0.0,
        /*stop_loss=*/std::nullopt, /*take_profit=*/95.0);
    engine eng(make_pathless_intrabar_series(100.0, 110.0, 90.0, 100.0),
               nullptr, strategy, make_pathless_intrabar_config());
    eng.run();

    expect_ambiguous_bar_rejected_without_economic_mutation(eng);
}

TEST(EngineLookahead,
     C01_PathlessNonMarketableBuyStopLimitRequiresPostTriggerRetrace)
{
    SilenceCout quiet;

    // O-H-L-C triggers at 105 and later retraces through limit 103, whereas
    // O-L-H-C reaches 103 only before the trigger. Identical pathless OHLC
    // cannot establish whether an economic entry fill exists at all.
    auto strategy = std::make_shared<IntrabarEntryWithProtectionStrategy>(
        order_type::stop_limit, order_side::buy,
        /*entry_price=*/103.0, /*trigger_price=*/105.0,
        /*stop_loss=*/std::nullopt, /*take_profit=*/std::nullopt);
    engine eng(make_pathless_intrabar_series(100.0, 110.0, 90.0, 108.0),
               nullptr, strategy, make_pathless_intrabar_config());
    eng.run();

    expect_ambiguous_bar_rejected_without_economic_mutation(eng);
}

TEST(EngineLookahead,
     C01_PathlessNonMarketableSellStopLimitRequiresPostTriggerRetrace)
{
    SilenceCout quiet;

    // Mirror case: only O-L-H-C supplies a post-trigger retrace through 97.
    auto strategy = std::make_shared<IntrabarEntryWithProtectionStrategy>(
        order_type::stop_limit, order_side::sell,
        /*entry_price=*/97.0, /*trigger_price=*/95.0,
        /*stop_loss=*/std::nullopt, /*take_profit=*/std::nullopt);
    engine eng(make_pathless_intrabar_series(100.0, 110.0, 90.0, 92.0),
               nullptr, strategy, make_pathless_intrabar_config());
    eng.run();

    expect_ambiguous_bar_rejected_without_economic_mutation(eng);
}

TEST(EngineLookahead,
     C01_PathlessPartialFillIsRejectedBeforeAnyEconomicMutation)
{
    SilenceCout quiet;

    // Volume limits the resting buy to a partial fill. Atomic fail-closed
    // handling must reject before that partial quantity reaches any ledger.
    auto strategy = std::make_shared<IntrabarEntryWithProtectionStrategy>(
        order_type::limit, order_side::buy,
        /*entry_price=*/95.0, /*trigger_price=*/0.0,
        /*stop_loss=*/std::nullopt, /*take_profit=*/105.0,
        /*quantity=*/2.0);
    engine eng(make_pathless_intrabar_series(
                   100.0, 110.0, 90.0, 100.0, /*ambiguous_volume=*/1.0),
               nullptr, strategy, make_pathless_intrabar_config());
    eng.run();

    expect_ambiguous_bar_rejected_without_economic_mutation(eng);
}

TEST(EngineLookahead,
     C01_UnambiguousRestingBuyLimitThenStopLossRemainsSupported)
{
    SilenceCout quiet;

    // Both canonical paths cross the resting entry before SL=90: O-L does so
    // on the downward leg; O-H-L does so after the high. This must not halt.
    auto strategy = std::make_shared<IntrabarEntryWithProtectionStrategy>(
        order_type::limit, order_side::buy,
        /*entry_price=*/95.0, /*trigger_price=*/0.0,
        /*stop_loss=*/90.0, /*take_profit=*/std::nullopt);
    engine eng(make_pathless_intrabar_series(100.0, 110.0, 85.0, 100.0),
               nullptr, strategy, make_pathless_intrabar_config());
    eng.run();

    expect_unambiguous_round_trip(eng, 85.0, 95.0);
}

TEST(EngineLookahead,
     C01_UnambiguousBuyStopThenTakeProfitRemainsSupported)
{
    SilenceCout quiet;

    // Both canonical paths reach trigger 105 before TP=108. The unrelated
    // low never creates a competing protected outcome.
    auto strategy = std::make_shared<IntrabarEntryWithProtectionStrategy>(
        order_type::stop, order_side::buy,
        /*entry_price=*/0.0, /*trigger_price=*/105.0,
        /*stop_loss=*/std::nullopt, /*take_profit=*/108.0);
    engine eng(make_pathless_intrabar_series(100.0, 110.0, 99.0, 107.0),
               nullptr, strategy, make_pathless_intrabar_config());
    eng.run();

    expect_unambiguous_round_trip(eng, 105.0, 111.0);
}

TEST(EngineLookahead,
     C01_BothTouchedAfterBuyStopHasCausallyPriorTakeProfit)
{
    SilenceCout quiet;

    // O-H-L reaches stop 105 then TP 108 before the low. O-L-H reaches the
    // low before entry, then stop 105 and TP 108. Both paths therefore agree
    // on TP; generic SL-first ordering is causally wrong for this new entry.
    auto strategy = std::make_shared<IntrabarEntryWithProtectionStrategy>(
        order_type::stop, order_side::buy,
        /*entry_price=*/0.0, /*trigger_price=*/105.0,
        /*stop_loss=*/95.0, /*take_profit=*/108.0);
    engine eng(make_pathless_intrabar_series(100.0, 110.0, 90.0, 100.0),
               nullptr, strategy, make_pathless_intrabar_config());
    eng.run();

    expect_unambiguous_round_trip(eng, 105.0, 111.0);
}
