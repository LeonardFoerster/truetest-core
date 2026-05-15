// Pins the look-ahead fix: a strategy that emits a market order on bar N must
// fill at bar N+1's open (not bar N's close) when execution_bar_delay=1,
// and fill at bar N's close when execution_bar_delay=0 (opt-out).

#include <gtest/gtest.h>
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "orderbook/orderbook.h"
#include "market_maker/market_maker.h"
#include "strategy/strategy_interface.h"

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
    cfg.seed = 1;                      // deterministic epoch-based timestamps
    cfg.initial_balance = 100000.0;
    ASSERT_EQ(cfg.execution_bar_delay, 1u) << "default must be 1 bar of delay";

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    auto report = eng.get_analytics().generate_report();
    ASSERT_EQ(report.trades.size(), 1u);
    const auto& t = report.trades.front();

    // Fill must have happened strictly AFTER bar 3's timestamp. Comparing at
    // time_point granularity (not millisecond-truncated) since earliest-
    // eligible is stamped as bar3_ts + 1ns.
    auto trigger_ts = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(strat->trigger_bar_index()));
    EXPECT_GT(t.timestamp, trigger_ts)
        << "order emitted on bar 3 must not fill within bar 3 under default bar-delay";

    // Fill price must reflect bar 4's open (~200), not bar 3's close (~100).
    // LocalBookAdapter adds a small aggression markup on market orders.
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

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    auto report = eng.get_analytics().generate_report();
    ASSERT_EQ(report.trades.size(), 1u);
    const auto& t = report.trades.front();

    // With delay disabled the fill should carry bar 3's timestamp (exactly)
    // and a price consistent with bar 3's close (~100).
    auto trigger_ts = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(strat->trigger_bar_index()));
    EXPECT_EQ(t.timestamp, trigger_ts);
    EXPECT_LT(t.fill_price, 150.0);
}
