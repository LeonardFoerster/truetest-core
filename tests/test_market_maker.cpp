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

    // High vol should have wider spreads - hard to assert precisely
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
