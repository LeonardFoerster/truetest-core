#include <gtest/gtest.h>
#include "orderbook/orderbook.h"

// Helper: construct Price from a dollar value
static Price P(double d) { return Price::from_double(d); }

TEST(Orderbook, AddOrder_SingleBid)
{
    orderbook ob;
    auto o = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100);
    ob.add_order(o);
    EXPECT_EQ(ob.size(), 1u);
}

TEST(Orderbook, AddOrder_SingleAsk)
{
    orderbook ob;
    auto o = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::sell, P(100.0), 100);
    ob.add_order(o);
    EXPECT_EQ(ob.size(), 1u);
}

TEST(Orderbook, MatchOrders_ExactCross)
{
    orderbook ob;
    auto bid = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100);
    ob.add_order(bid);
    auto ask = std::make_shared<order>(ob_order_type::good_till_cancel, 2, side::sell, P(100.0), 100);
    auto t = ob.add_order(ask);
    EXPECT_EQ(t.size(), 1u);
    EXPECT_EQ(ob.size(), 0u);
}

TEST(Orderbook, MatchOrders_BidHigherThanAsk)
{
    orderbook ob;
    auto bid = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(110.0), 100);
    ob.add_order(bid);
    auto ask = std::make_shared<order>(ob_order_type::good_till_cancel, 2, side::sell, P(100.0), 100);
    auto t = ob.add_order(ask);
    EXPECT_EQ(t.size(), 1u);
}

TEST(Orderbook, MatchOrders_NoMatch)
{
    orderbook ob;
    auto bid = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(90.0), 100);
    ob.add_order(bid);
    auto ask = std::make_shared<order>(ob_order_type::good_till_cancel, 2, side::sell, P(100.0), 100);
    auto t = ob.add_order(ask);
    EXPECT_EQ(t.size(), 0u);
    EXPECT_EQ(ob.size(), 2u);
}

TEST(Orderbook, MatchOrders_PartialFill)
{
    orderbook ob;
    auto bid = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100);
    ob.add_order(bid);
    auto ask = std::make_shared<order>(ob_order_type::good_till_cancel, 2, side::sell, P(100.0), 50);
    auto t = ob.add_order(ask);
    EXPECT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].get_bid_trade().quantity_, 50u);
    EXPECT_EQ(bid->get_remaining_quantity(), 50u);
}

TEST(Orderbook, MatchOrders_MultipleFills)
{
    orderbook ob;
    auto bid = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100);
    ob.add_order(bid);

    for (int i = 0; i < 3; ++i)
    {
        auto ask = std::make_shared<order>(ob_order_type::good_till_cancel, 10 + i, side::sell, P(100.0), 30);
        ob.add_order(ask);
    }
    EXPECT_EQ(bid->get_remaining_quantity(), 10u);
}

TEST(Orderbook, DuplicateOrderId)
{
    orderbook ob;
    auto o1 = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100);
    ob.add_order(o1);
    auto o2 = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 50);
    auto t = ob.add_order(o2);
    EXPECT_TRUE(t.empty());
}

TEST(Orderbook, CancelOrder)
{
    orderbook ob;
    auto o = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100);
    ob.add_order(o);
    ob.cancel_order(1);
}

TEST(Orderbook, ModifyOrder_ReplacesRestingOrderWithoutChangingIdentity)
{
    orderbook ob;
    auto original = std::make_shared<order>(
        ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100);
    ob.add_order(original);

    ASSERT_TRUE(ob.modify_order(1, P(99.0), 40));

    const auto modified = ob.get_order(1);
    ASSERT_NE(modified, nullptr);
    EXPECT_NE(modified, original);
    EXPECT_EQ(modified->get_order_id(), 1u);
    EXPECT_EQ(modified->get_order_type(), ob_order_type::good_till_cancel);
    EXPECT_EQ(modified->get_side(), side::buy);
    EXPECT_EQ(modified->get_price(), P(99.0));
    EXPECT_EQ(modified->get_remaining_quantity(), 40u);

    const auto levels = ob.get_order_infos();
    ASSERT_EQ(levels.get_bids().size(), 1u);
    EXPECT_EQ(levels.get_bids()[0].price_, P(99.0));
    EXPECT_EQ(levels.get_bids()[0].quantity_, 40u);
}

TEST(Orderbook, ModifyOrder_UnknownIdLeavesBookUnchanged)
{
    orderbook ob;
    ob.add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 1, side::sell, P(101.0), 25));

    EXPECT_FALSE(ob.modify_order(999, P(102.0), 10));

    const auto original = ob.get_order(1);
    ASSERT_NE(original, nullptr);
    EXPECT_EQ(original->get_price(), P(101.0));
    EXPECT_EQ(original->get_remaining_quantity(), 25u);
    EXPECT_EQ(ob.size(), 1u);
}

TEST(Orderbook, FillOrKill_CanMatch)
{
    orderbook ob;
    auto ask = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::sell, P(100.0), 100);
    ob.add_order(ask);
    auto fok = std::make_shared<order>(ob_order_type::fill_or_kill, 2, side::buy, P(100.0), 100);
    auto t = ob.add_order(fok);
    EXPECT_EQ(t.size(), 1u);
}

TEST(Orderbook, FillOrKill_CannotMatch)
{
    orderbook ob;
    auto fok = std::make_shared<order>(ob_order_type::fill_or_kill, 1, side::buy, P(100.0), 100);
    auto t = ob.add_order(fok);
    EXPECT_TRUE(t.empty());
}

TEST(Order, Fill_Overfill)
{
    order o(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 10);
    EXPECT_THROW(o.fill(11), std::logic_error);
}

TEST(Order, Fill_Exact)
{
    order o(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 10);
    o.fill(10);
    EXPECT_TRUE(o.is_filled());
    EXPECT_EQ(o.get_filled_quantity(), 10u);
}

TEST(Orderbook, GetOrderInfos)
{
    orderbook ob;
    ob.add_order(std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(99.0), 50));
    ob.add_order(std::make_shared<order>(ob_order_type::good_till_cancel, 2, side::sell, P(101.0), 30));
    auto info = ob.get_order_infos();
    EXPECT_FALSE(info.get_bids().empty());
    EXPECT_FALSE(info.get_asks().empty());
}

TEST(Orderbook, L2Snapshot_Apply)
{
    orderbook ob;
    std::vector<std::pair<Price, quantity>> bids = {{P(100.0), 100}, {P(99.0), 200}};
    std::vector<std::pair<Price, quantity>> asks = {{P(101.0), 150}};
    ob.apply_l2_snapshot(bids, asks);
    EXPECT_EQ(ob.size(), 3u);
}

TEST(Orderbook, L2Snapshot_ClearsPrevious)
{
    orderbook ob;
    ob.add_order(std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(50.0), 10));
    EXPECT_EQ(ob.size(), 1u);
    std::vector<std::pair<Price, quantity>> bids = {{P(100.0), 100}};
    std::vector<std::pair<Price, quantity>> asks = {};
    ob.apply_l2_snapshot(bids, asks);
    EXPECT_EQ(ob.size(), 1u);
}

TEST(Orderbook, L2Update_AddLevel)
{
    orderbook ob;
    ob.apply_l2_update(side::buy, P(100.0), 100);
    auto info = ob.get_order_infos();
    EXPECT_FALSE(info.get_bids().empty());
}

TEST(Orderbook, L2Update_RemoveLevel)
{
    orderbook ob;
    ob.apply_l2_update(side::sell, P(101.0), 100);
    ob.apply_l2_update(side::sell, P(101.0), 0);
    auto info = ob.get_order_infos();
    bool found = false;
    for (const auto& lvl : info.get_asks())
        if (lvl.price_ == P(101.0)) found = true;
    EXPECT_FALSE(found);
}

TEST(Orderbook, Clear)
{
    orderbook ob;
    ob.add_order(std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100));
    ob.add_order(std::make_shared<order>(ob_order_type::good_till_cancel, 2, side::sell, P(101.0), 100));
    ob.clear();
    EXPECT_EQ(ob.size(), 0u);
}
