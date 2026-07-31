// Integration: platform DefaultExitPolicy arms SL for strategies that emit
// no exit_intents. Pins floor vs strategy_only and reducing-close safety.

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

// Buys once on first bar; never emits exit intents or signal closes.
class DumbBuyOnce : public IStrategy
{
    bool fired_ = false;
public:
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        if (fired_) return std::nullopt;
        fired_ = true;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy,
                           1.0, mkt.get_close());
    }
    void set_position_open(const std::string&, bool) override {}
};

// Climb then crash through 1% SL from entry ~100.
std::shared_ptr<data_handler> make_crash_after_entry()
{
    auto dh = std::make_shared<data_handler>();
    // Warm bars around 100
    for (int i = 0; i < 3; ++i)
        dh->load_into_queue("2024-01-01", "TEST", 100.0, 101.0, 99.0, 100.0, 1000);
    // Entry bar close 100
    dh->load_into_queue("2024-01-01", "TEST", 100.0, 101.0, 99.0, 100.0, 1000);
    // Next open ~100 (fill under delay=1), then crash: low 90
    dh->load_into_queue("2024-01-01", "TEST", 100.0, 101.0, 90.0, 91.0, 1000);
    dh->load_into_queue("2024-01-01", "TEST", 91.0, 92.0, 90.0, 91.0, 1000);
    return dh;
}

} // namespace

TEST(DefaultExitPolicyEngine, FloorArmsSlForDumbStrategy)
{
    SilenceCout quiet;
    auto dh = make_crash_after_entry();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<DumbBuyOnce>();
    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.seed = 1;
    cfg.initial_balance = 100000.0;
    cfg.execution_bar_delay = 1;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::floor;
    cfg.exit_defaults.sl_pct = 0.01;  // SL at 99
    cfg.exit_defaults.tp_pct = 0.50;  // far TP so SL fires first

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    auto report = eng.get_analytics().generate_report();
    // Entry + SL close at least
    ASSERT_GE(report.trades.size(), 2u) << "platform SL must close the dumb long";
    bool saw_sell = false;
    for (const auto& t : report.trades)
        if (t.side == order_side::sell)
            saw_sell = true;
    EXPECT_TRUE(saw_sell);
}

TEST(DefaultExitPolicyEngine, StrategyOnlyNoPlatformSl)
{
    SilenceCout quiet;
    auto dh = make_crash_after_entry();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<DumbBuyOnce>();
    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.seed = 1;
    cfg.initial_balance = 100000.0;
    cfg.execution_bar_delay = 1;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    cfg.exit_defaults.sl_pct = 0.01;
    cfg.exit_defaults.tp_pct = 0.50;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    auto report = eng.get_analytics().generate_report();
    // Only the entry buy — no platform close
    ASSERT_EQ(report.trades.size(), 1u);
    EXPECT_EQ(report.trades.front().side, order_side::buy);
}

// Buy once then signal-close (sell) while long; floor must not arm a short
// bracket on the reducing sell (no inverted protection after flat).
class BuyThenSignalSell : public IStrategy
{
    int bars_ = 0;
    bool bought_ = false;
    bool sold_ = false;
public:
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        ++bars_;
        if (!bought_ && bars_ == 1)
        {
            bought_ = true;
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::buy,
                               1.0, mkt.get_close());
        }
        // After entry has had a chance to fill (delay=1 → bar 2), signal sell.
        if (bought_ && !sold_ && bars_ >= 3)
        {
            sold_ = true;
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::sell,
                               1.0, mkt.get_close());
        }
        return std::nullopt;
    }
    void set_position_open(const std::string&, bool) override {}
};

TEST(DefaultExitPolicyEngine, FloorSignalCloseDoesNotArmInvertedShort)
{
    SilenceCout quiet;
    auto dh = std::make_shared<data_handler>();
    // Quiet path ~100 so entry platform SL/TP (set very wide below) never fire.
    for (int i = 0; i < 6; ++i)
        dh->load_into_queue("2024-01-01", "TEST", 100.0, 100.5, 99.5, 100.0, 1000);
    // After signal close is flat, spike hard up — inverted short SL would fire.
    dh->load_into_queue("2024-01-01", "TEST", 100.0, 130.0, 99.5, 125.0, 1000);
    dh->load_into_queue("2024-01-01", "TEST", 125.0, 140.0, 124.0, 135.0, 1000);

    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<BuyThenSignalSell>();
    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.seed = 1;
    cfg.initial_balance = 100000.0;
    cfg.execution_bar_delay = 1;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::floor;
    // Wide enough that normal 100±0.5 bars cannot hit entry SL/TP.
    cfg.exit_defaults.sl_pct = 0.20;
    cfg.exit_defaults.tp_pct = 0.50;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    auto report = eng.get_analytics().generate_report();
    // Entry buy + signal sell only. No third fill from inverted short SL on the spike.
    ASSERT_EQ(report.trades.size(), 2u)
        << "signal close must not spawn inverted platform entry; got "
        << report.trades.size() << " fills";
    EXPECT_EQ(report.trades[0].side, order_side::buy);
    EXPECT_EQ(report.trades[1].side, order_side::sell);
}
