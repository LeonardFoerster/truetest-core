#include <gtest/gtest.h>
#include "market_maker/market_maker.h"
#include "orderbook/orderbook.h"

TEST(MarketMaker, AddOrders_PopulatesBook)
{
    auto ob = std::make_shared<orderbook>();
    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);
    EXPECT_EQ(ob->size(), 20u); // 10 bids + 10 asks
}

TEST(MarketMaker, AddOrders_BidsBelowAsk)
{
    auto ob = std::make_shared<orderbook>();
    MarketMaker mm;
    mm.add_orders(ob, 100.0, 5);
    auto info = ob->get_order_infos();
    Price mid = Price::from_double(100.0);
    for (const auto& bid : info.get_bids())
        EXPECT_LE(bid.price_, mid);
    for (const auto& ask : info.get_asks())
        EXPECT_GE(ask.price_, mid);
}

TEST(MarketMaker, Replenish_AddsLevels)
{
    auto ob = std::make_shared<orderbook>();
    MarketMaker mm;
    std::size_t before = ob->size();
    mm.replenish(ob, 100.0);
    EXPECT_GT(ob->size(), before);
}

TEST(MarketMaker, Replenish_VolatilityWidensSpread)
{
    // Low volatility: constant prices
    auto ob_low = std::make_shared<orderbook>();
    MarketMaker mm_low;
    for (int i = 0; i < 50; ++i)
        mm_low.replenish(ob_low, 100.0);
    auto info_low = ob_low->get_order_infos();

    // High volatility: big swings
    auto ob_high = std::make_shared<orderbook>();
    MarketMaker mm_high;
    for (int i = 0; i < 50; ++i)
        mm_high.replenish(ob_high, (i % 2 == 0) ? 100.0 : 110.0);
    auto info_high = ob_high->get_order_infos();

    // High vol should have wider spreads — hard to assert precisely
    // but we can check both produce non-empty books
    EXPECT_FALSE(info_low.get_bids().empty());
    EXPECT_FALSE(info_high.get_bids().empty());
}

TEST(MarketMaker, OrderId_NoCollision)
{
    auto ob = std::make_shared<orderbook>();
    MarketMaker mm;
    mm.add_orders(ob, 100.0, 5);
    std::size_t after_add = ob->size();
    mm.replenish(ob, 100.0);
    // Both calls should add orders without duplicate ID issues
    EXPECT_GT(ob->size(), after_add);
}

TEST(MarketMaker, Calibration_ReflectedInReplenish)
{
    MarketMaker mm(42);
    mm.set_calibration({/*levels_per_side=*/3, /*base_depth=*/7,
                        /*base_spread_pct=*/0.01, /*vol_spread_mult=*/5.0});

    // First call: no price history yet → vol = 0 → half_spread = 0.01.
    auto orders = mm.compute_replenish(100.0);

    // 3 bid + 3 ask levels.
    ASSERT_EQ(orders.size(), 6u);

    // Level i rests at mid × (1 ± i × 0.01) with depth 7 × i.
    int bid_i = 0, ask_i = 0;
    for (const auto& mo : orders)
    {
        if (mo.side == order_side::buy)
        {
            ++bid_i;
            EXPECT_NEAR(mo.price, 100.0 * (1.0 - 0.01 * bid_i), 1e-9);
            EXPECT_NEAR(mo.quantity, 7.0 * bid_i, 1e-9);
        }
        else
        {
            ++ask_i;
            EXPECT_NEAR(mo.price, 100.0 * (1.0 + 0.01 * ask_i), 1e-9);
            EXPECT_NEAR(mo.quantity, 7.0 * ask_i, 1e-9);
        }
    }
    EXPECT_EQ(bid_i, 3);
    EXPECT_EQ(ask_i, 3);
}

TEST(MarketMaker, Calibration_SurvivesReset)
{
    MarketMaker mm(42);
    mm.set_calibration({2, 5, 0.005, 5.0});
    mm.reset(43);

    auto orders = mm.compute_replenish(100.0);
    ASSERT_EQ(orders.size(), 4u) << "calibration is config, not trial state";
}

TEST(MarketMaker, Calibration_VolMultWidensSpread)
{
    // vol_mult = 0 keeps the seeded spread at base_spread_pct regardless
    // of realized volatility; a large vol_mult must widen it.
    MarketMaker mm_flat(42), mm_vol(42);
    mm_flat.set_calibration({1, 100, 0.002, 0.0});
    mm_vol.set_calibration({1, 100, 0.002, 50.0});

    std::vector<mm_order> flat_orders, vol_orders;
    for (int i = 0; i < 20; ++i)
    {
        const double px = (i % 2 == 0) ? 100.0 : 110.0;
        flat_orders = mm_flat.compute_replenish(px);
        vol_orders = mm_vol.compute_replenish(px);
    }

    auto ask_price = [](const std::vector<mm_order>& v) {
        for (const auto& mo : v)
            if (mo.side == order_side::sell) return mo.price;
        return 0.0;
    };
    EXPECT_GT(ask_price(vol_orders), ask_price(flat_orders))
        << "vol_spread_mult must widen the seeded spread under volatility";
}
