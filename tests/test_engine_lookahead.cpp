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
#include "market_maker/market_maker.h"
#include "strategy/ma_crossover_strategy.h"
#include "strategy/strategy_interface.h"

#include <chrono>
#include <sstream>

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
