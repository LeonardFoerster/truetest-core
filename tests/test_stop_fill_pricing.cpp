// Pins the stop-fill pricing convention: a triggered stop fills anchored
// at the STOP price (or at the bar OPEN when the bar gaps through the
// stop) — never at the bar close, which is intra-bar look-ahead. The
// engine re-centers the synthetic book at the trigger reference before
// converting the stop, so the fill lands within the calibrated seeded
// spread of that reference.
//
// Calibration in these tests: mm_levels=1, vol_mult=0, spread_pct=0.002
// → exactly one ask at ref × 1.002 and one bid at ref × 0.998.

#include <gtest/gtest.h>
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "strategy/strategy_interface.h"

#include <array>
#include <cstdint>
#include <sstream>
#include <vector>

namespace {

struct SilenceCout {
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceCout() : orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~SilenceCout() { std::cout.rdbuf(orig); }
};

// Places one stop (or stop-limit) order on the first bar. Fill prices
// are read from the analytics report (per-strategy on_fill dispatch is
// name-based and skips unnamed test strategies).
class StopPlacer : public IStrategy
{
    int call_count_ = 0;
    order_type type_;
    order_side side_;
    double stop_price_;
    double limit_price_;
public:
    StopPlacer(order_type type, order_side side, double stop_price,
               double limit_price = 0.0)
        : type_(type), side_(side), stop_price_(stop_price),
          limit_price_(limit_price) {}

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        if (call_count_++ == 0)
            return order_event(mkt.get_timestamp(), mkt.get_symbol(), type_,
                               side_, 10, limit_price_, time_in_force::gtc,
                               stop_price_);
        return std::nullopt;
    }

    void set_position_open(const std::string&, bool) override {}
};

double first_fill_price(engine& eng)
{
    auto report = eng.get_analytics().generate_report();
    if (report.trades.empty()) return 0.0;
    return report.trades.front().fill_price;
}

engine_config make_cfg()
{
    engine_config cfg;
    cfg.seed = 1;
    cfg.initial_balance = 100000.0;
    // One deterministic level per side at ref × (1 ± 0.002).
    cfg.mm_levels_per_side = 1;
    cfg.mm_base_spread_pct = 0.002;
    cfg.mm_vol_spread_mult = 0.0;
    return cfg;
}

std::shared_ptr<data_handler> make_bars(
    const std::vector<std::array<double, 4>>& ohlc)
{
    auto dh = std::make_shared<data_handler>();
    for (const auto& b : ohlc)
        dh->load_into_queue("2024-01-01", "TEST", b[0], b[1], b[2], b[3], 1000);
    return dh;
}

} // namespace

// Stop buy 104, trigger bar opens at 103 (below the stop) and closes at
// 114: the fill must anchor at the stop (104 × 1.002), not at the close
// (which would be ≈ 114.2).
TEST(StopFillPricing, BuyStopFillsAtStopNotClose)
{
    SilenceCout quiet;
    auto dh = make_bars({{100, 101, 99, 100},
                         {100, 101, 99, 100},
                         {103, 115, 102, 114}});
    auto strat = std::make_shared<StopPlacer>(
        order_type::stop, order_side::buy, 104.0);

    engine eng(dh, nullptr, strat, make_cfg());
    eng.run();

    const double px = first_fill_price(eng);
    ASSERT_GT(px, 0.0) << "stop must trigger and fill";
    EXPECT_NEAR(px, 104.0 * 1.002, 1e-3)
        << "fill anchored at the stop price, not the bar close";
}

// Trigger bar gaps through the stop (opens at 108 > 104): the fill
// anchors at the open — the stop could not have filled better than where
// the market opened.
TEST(StopFillPricing, BuyStopGapThroughFillsAtOpen)
{
    SilenceCout quiet;
    auto dh = make_bars({{100, 101, 99, 100},
                         {100, 101, 99, 100},
                         {108, 112, 107, 111}});
    auto strat = std::make_shared<StopPlacer>(
        order_type::stop, order_side::buy, 104.0);

    engine eng(dh, nullptr, strat, make_cfg());
    eng.run();

    const double px = first_fill_price(eng);
    ASSERT_GT(px, 0.0) << "stop must trigger and fill";
    EXPECT_NEAR(px, 108.0 * 1.002, 1e-3)
        << "gap-through buy stop fills at the open, not at the stop";
}

// Sell-side mirror: stop sell 96, trigger bar opens at 97 (above) and
// closes at 86 — fill anchors at 96 × 0.998, not ≈ 85.8.
TEST(StopFillPricing, SellStopFillsAtStopNotClose)
{
    SilenceCout quiet;
    auto dh = make_bars({{100, 101, 99, 100},
                         {100, 101, 99, 100},
                         {97, 98, 85, 86}});
    auto strat = std::make_shared<StopPlacer>(
        order_type::stop, order_side::sell, 96.0);

    engine eng(dh, nullptr, strat, make_cfg());
    eng.run();

    const double px = first_fill_price(eng);
    ASSERT_GT(px, 0.0) << "stop must trigger and fill";
    EXPECT_NEAR(px, 96.0 * 0.998, 1e-3)
        << "fill anchored at the stop price, not the bar close";
}

TEST(StopFillPricing, SellStopGapThroughFillsAtOpen)
{
    SilenceCout quiet;
    auto dh = make_bars({{100, 101, 99, 100},
                         {100, 101, 99, 100},
                         {92, 93, 84, 85}});
    auto strat = std::make_shared<StopPlacer>(
        order_type::stop, order_side::sell, 96.0);

    engine eng(dh, nullptr, strat, make_cfg());
    eng.run();

    const double px = first_fill_price(eng);
    ASSERT_GT(px, 0.0) << "stop must trigger and fill";
    EXPECT_NEAR(px, 92.0 * 0.998, 1e-3)
        << "gap-through sell stop fills at the open, not at the stop";
}

// Stop-limit: trigger and book anchor work exactly like the market stop;
// the converted limit (106) is marketable against the 104-anchored ask.
TEST(StopFillPricing, StopLimitAnchoredAtTrigger)
{
    SilenceCout quiet;
    auto dh = make_bars({{100, 101, 99, 100},
                         {100, 101, 99, 100},
                         {103, 115, 102, 114}});
    auto strat = std::make_shared<StopPlacer>(
        order_type::stop_limit, order_side::buy, 104.0, /*limit=*/106.0);

    engine eng(dh, nullptr, strat, make_cfg());
    eng.run();

    const double px = first_fill_price(eng);
    ASSERT_GT(px, 0.0) << "stop must trigger and fill";
    EXPECT_NEAR(px, 104.0 * 1.002, 1e-3)
        << "stop-limit fills against the trigger-anchored book";
}
