#include <gtest/gtest.h>
#include "orderbook/orderbook.h"

#include <array>
#include <limits>

// Helper: construct Price from a dollar value
static Price P(double d)
{
    return Price::from_double(d);
}

TEST(Orderbook, AddOrder_SingleBid)
{
    orderbook ob;
    auto o = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100);
    ob.add_order(o);
    EXPECT_EQ(ob.size(), 1u);
}

TEST(Orderbook, L2SnapshotReplacementPreservesLocallyRestingOrder)
{
    orderbook ob;
    ob.apply_l2_snapshot({{P(100.0), 100}}, {{P(101.0), 100}});
    constexpr order_id local_id = 9'000'001;
    auto local = ob.create_order(ob_order_type::good_till_cancel, local_id, side::buy, P(99.0), 5);
    EXPECT_TRUE(ob.add_order(local).empty());
    ASSERT_NE(ob.get_order(local_id), nullptr);

    ob.apply_l2_snapshot({{P(98.0), 200}}, {{P(102.0), 300}});

    auto preserved = ob.get_order(local_id);
    ASSERT_NE(preserved, nullptr);
    EXPECT_EQ(preserved, local);
    EXPECT_EQ(preserved->get_remaining_quantity(), 5u);
    ob.cancel_order(local_id);
    EXPECT_EQ(ob.get_order(local_id), nullptr);
}

TEST(Orderbook, L2UpdateAtSharedPriceReplacesOnlyExternalLiquidity)
{
    orderbook ob;
    ob.apply_l2_snapshot({{P(100.0), 100}}, {{P(101.0), 100}});
    constexpr order_id local_id = 9'000'002;
    auto local = ob.create_order(ob_order_type::good_till_cancel, local_id, side::buy, P(100.0), 5);
    EXPECT_TRUE(ob.add_order(local).empty());

    ob.apply_l2_update(side::buy, P(100.0), 200);
    ASSERT_EQ(ob.get_order(local_id), local);
    auto levels = ob.get_order_infos();
    ASSERT_FALSE(levels.get_bids().empty());
    EXPECT_EQ(levels.get_bids().front().quantity_, 205u);

    ob.apply_l2_update(side::buy, P(100.0), 0);
    ASSERT_EQ(ob.get_order(local_id), local);
    levels = ob.get_order_infos();
    ASSERT_FALSE(levels.get_bids().empty());
    EXPECT_EQ(levels.get_bids().front().quantity_, 5u);
}

TEST(Orderbook, ExternalSnapshotViewExcludesRestingStrategyOrders)
{
    orderbook ob;
    ob.apply_l2_snapshot({{P(100.0), 100}}, {{P(101.0), 120}});
    auto local =
        ob.create_order(ob_order_type::good_till_cancel, 9'000'003, side::buy, P(100.0), 5);
    ASSERT_TRUE(ob.add_order(local).empty());

    const auto all = ob.get_order_infos();
    const auto external = ob.get_external_order_infos();
    ASSERT_EQ(all.get_bids().size(), 1u);
    ASSERT_EQ(external.get_bids().size(), 1u);
    EXPECT_EQ(all.get_bids().front().quantity_, 105u);
    EXPECT_EQ(external.get_bids().front().quantity_, 100u);
    ASSERT_EQ(external.get_asks().size(), 1u);
    EXPECT_EQ(external.get_asks().front().quantity_, 120u);
}

TEST(Orderbook, ExternalDepthPrefixIsBoundedButReportsAllLevels)
{
    orderbook ob;
    ob.apply_l2_snapshot(
        {{P(100.0), 10}, {P(99.0), 20}, {P(98.0), 30}},
        {{P(101.0), 40}, {P(102.0), 50}, {P(103.0), 60}});
    std::array<lvl_info, 2> bids{};
    std::array<lvl_info, 1> asks{};
    const auto result = ob.copy_external_depth(bids, asks);

    EXPECT_FALSE(result.quantity_overflow);
    EXPECT_EQ(result.bid_count, 2u);
    EXPECT_EQ(result.ask_count, 1u);
    EXPECT_EQ(result.total_bid_levels, 3u);
    EXPECT_EQ(result.total_ask_levels, 3u);
    EXPECT_DOUBLE_EQ(bids[0].price_.to_double(), 100.0);
    EXPECT_EQ(bids[0].quantity_, 10u);
    EXPECT_DOUBLE_EQ(asks[0].price_.to_double(), 101.0);
}

TEST(Orderbook, PriceLevelRejectsQuantityOverflowWithoutMutation)
{
    price_level level{.price = P(100.0)};
    auto max_order = std::make_shared<order>(ob_order_type::good_till_cancel,
                                             1, side::buy, P(100.0),
                                             std::numeric_limits<quantity>::max());
    auto one_order = std::make_shared<order>(ob_order_type::good_till_cancel,
                                             2, side::buy, P(100.0), 1);
    order_node first{.order = max_order};
    order_node second{.order = one_order};
    ASSERT_TRUE(level.append(&first));
    EXPECT_FALSE(level.append(&second));
    EXPECT_EQ(level.total_qty, std::numeric_limits<quantity>::max());
    EXPECT_EQ(level.head, &first);
    EXPECT_EQ(level.tail, &first);
    EXPECT_EQ(second.prev, nullptr);
    EXPECT_EQ(second.next, nullptr);
}

TEST(Orderbook, L2SnapshotOverflowRejectsWholeReplacement)
{
    orderbook ob;
    ASSERT_EQ(ob.apply_l2_snapshot(
                  {{P(100.0), 10}}, {{P(101.0), 20}}),
              l2_apply_status::applied);

    const std::vector<std::pair<Price, quantity>> overflowing_bids{
        {P(99.0), std::numeric_limits<quantity>::max()},
        {P(99.0), 1}};
    const std::vector<std::pair<Price, quantity>> replacement_asks{
        {P(102.0), 30}};
    EXPECT_EQ(ob.apply_l2_snapshot(overflowing_bids, replacement_asks),
              l2_apply_status::quantity_overflow);

    const auto external = ob.get_external_order_infos();
    ASSERT_EQ(external.get_bids().size(), 1u);
    EXPECT_EQ(external.get_bids()[0].price_, P(100.0));
    EXPECT_EQ(external.get_bids()[0].quantity_, 10u);
    ASSERT_EQ(external.get_asks().size(), 1u);
    EXPECT_EQ(external.get_asks()[0].price_, P(101.0));
    EXPECT_EQ(external.get_asks()[0].quantity_, 20u);
}

TEST(Orderbook, L2UpdateOverflowPreservesExternalAndLocalDepth)
{
    orderbook ob;
    ASSERT_EQ(ob.apply_l2_snapshot(
                  {{P(100.0), 10}}, {{P(101.0), 20}}),
              l2_apply_status::applied);
    constexpr order_id local_id = 9'000'004;
    auto local = ob.create_order(
        ob_order_type::good_till_cancel, local_id, side::buy, P(100.0),
        std::numeric_limits<quantity>::max() - 10);
    ASSERT_TRUE(ob.add_order(local).empty());

    EXPECT_EQ(ob.apply_l2_update(side::buy, P(100.0), 11),
              l2_apply_status::quantity_overflow);

    ASSERT_EQ(ob.get_order(local_id), local);
    const auto all = ob.get_order_infos();
    ASSERT_EQ(all.get_bids().size(), 1u);
    EXPECT_EQ(all.get_bids()[0].quantity_,
              std::numeric_limits<quantity>::max());
    const auto external = ob.get_external_order_infos();
    ASSERT_EQ(external.get_bids().size(), 1u);
    EXPECT_EQ(external.get_bids()[0].quantity_, 10u);
    ASSERT_EQ(external.get_asks().size(), 1u);
    EXPECT_EQ(external.get_asks()[0].quantity_, 20u);
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
    auto bid =
        std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100);
    ob.add_order(bid);
    auto ask =
        std::make_shared<order>(ob_order_type::good_till_cancel, 2, side::sell, P(100.0), 100);
    auto t = ob.add_order(ask);
    EXPECT_EQ(t.size(), 1u);
    EXPECT_EQ(ob.size(), 0u);
}

TEST(Orderbook, MatchOrders_BidHigherThanAsk)
{
    orderbook ob;
    auto bid =
        std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(110.0), 100);
    ob.add_order(bid);
    auto ask =
        std::make_shared<order>(ob_order_type::good_till_cancel, 2, side::sell, P(100.0), 100);
    auto t = ob.add_order(ask);
    EXPECT_EQ(t.size(), 1u);
}

TEST(Orderbook, MatchOrders_NoMatch)
{
    orderbook ob;
    auto bid = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(90.0), 100);
    ob.add_order(bid);
    auto ask =
        std::make_shared<order>(ob_order_type::good_till_cancel, 2, side::sell, P(100.0), 100);
    auto t = ob.add_order(ask);
    EXPECT_EQ(t.size(), 0u);
    EXPECT_EQ(ob.size(), 2u);
}

TEST(Orderbook, MatchOrders_PartialFill)
{
    orderbook ob;
    auto bid =
        std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100);
    ob.add_order(bid);
    auto ask =
        std::make_shared<order>(ob_order_type::good_till_cancel, 2, side::sell, P(100.0), 50);
    auto t = ob.add_order(ask);
    EXPECT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].get_bid_trade().quantity_, 50u);
    EXPECT_EQ(bid->get_remaining_quantity(), 50u);
}

TEST(Orderbook, MatchOrders_MultipleFills)
{
    orderbook ob;
    auto bid =
        std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100);
    ob.add_order(bid);

    for (int i = 0; i < 3; ++i) {
        auto ask = std::make_shared<order>(ob_order_type::good_till_cancel, 10 + i, side::sell,
                                           P(100.0), 30);
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
    auto original =
        std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100);
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
    ob.add_order(
        std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::sell, P(101.0), 25));

    EXPECT_FALSE(ob.modify_order(999, P(102.0), 10));

    const auto original = ob.get_order(1);
    ASSERT_NE(original, nullptr);
    EXPECT_EQ(original->get_price(), P(101.0));
    EXPECT_EQ(original->get_remaining_quantity(), 25u);
    EXPECT_EQ(ob.size(), 1u);
}

TEST(Orderbook, ModifyOrder_PoolExhaustionPreservesOriginalOrder)
{
    orderbook ob;
    ob.configure_order_pool(nullptr, 1, true);
    auto original = ob.create_order(
        ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100);
    ASSERT_TRUE(ob.add_order(original).empty());

    std::vector<order_pointer> held;
    held.reserve(4095);
    for (order_id id = 2; id <= 4096; ++id)
        held.push_back(ob.create_order(
            ob_order_type::good_till_cancel, id, side::sell, P(200.0), 1));

    EXPECT_THROW((void)ob.modify_order(1, P(99.0), 40), pool_exhausted);
    ASSERT_EQ(ob.get_order(1), original);
    EXPECT_EQ(original->get_price(), P(100.0));
    EXPECT_EQ(original->get_remaining_quantity(), 100u);
    const auto levels = ob.get_order_infos();
    ASSERT_EQ(levels.get_bids().size(), 1u);
    EXPECT_EQ(levels.get_bids()[0].price_, P(100.0));
    EXPECT_EQ(levels.get_bids()[0].quantity_, 100u);
    EXPECT_EQ(ob.size(), 1u);
}

TEST(Orderbook, ModifyOrder_OverflowPreservesOriginalOrder)
{
    orderbook ob;
    auto target = std::make_shared<order>(
        ob_order_type::good_till_cancel, 1, side::buy, P(99.0),
        std::numeric_limits<quantity>::max());
    auto original = std::make_shared<order>(
        ob_order_type::good_till_cancel, 2, side::buy, P(100.0), 1);
    ASSERT_TRUE(ob.add_order(target).empty());
    ASSERT_TRUE(ob.add_order(original).empty());

    EXPECT_FALSE(ob.modify_order(2, P(99.0), 1));
    EXPECT_EQ(ob.get_order(2), original);
    EXPECT_EQ(original->get_price(), P(100.0));
    EXPECT_EQ(original->get_remaining_quantity(), 1u);
    EXPECT_EQ(ob.size(), 2u);
}

TEST(Orderbook, GtcCapacityCheckUsesPostExternalMatchRemainder)
{
    orderbook ob;
    auto resting = std::make_shared<order>(
        ob_order_type::good_till_cancel, 1, side::buy, P(100.0),
        std::numeric_limits<quantity>::max());
    ASSERT_TRUE(ob.add_order(resting).empty());
    ASSERT_EQ(ob.apply_l2_update(side::sell, P(100.0), 1),
              l2_apply_status::applied);

    auto incoming = std::make_shared<order>(
        ob_order_type::good_till_cancel, 2, side::buy, P(100.0), 1);
    const auto matches = ob.add_order_against_external(incoming);

    ASSERT_EQ(matches.size(), 1u);
    EXPECT_TRUE(incoming->is_filled());
    EXPECT_EQ(ob.get_order(1), resting);
    const auto levels = ob.get_order_infos();
    ASSERT_EQ(levels.get_bids().size(), 1u);
    EXPECT_EQ(levels.get_bids()[0].quantity_,
              std::numeric_limits<quantity>::max());
}

TEST(Orderbook, GtcCapacityRejectsPredictedRemainderBeforeTakingDepth)
{
    orderbook ob;
    auto resting = std::make_shared<order>(
        ob_order_type::good_till_cancel, 1, side::buy, P(100.0),
        std::numeric_limits<quantity>::max());
    ASSERT_TRUE(ob.add_order(resting).empty());
    ASSERT_EQ(ob.apply_l2_update(side::sell, P(100.0), 1),
              l2_apply_status::applied);

    auto incoming = std::make_shared<order>(
        ob_order_type::good_till_cancel, 2, side::buy, P(100.0), 2);
    EXPECT_TRUE(ob.add_order_against_external(incoming).empty());
    EXPECT_EQ(incoming->get_remaining_quantity(), 2u);

    const auto external = ob.get_external_order_infos();
    ASSERT_EQ(external.get_asks().size(), 1u);
    EXPECT_EQ(external.get_asks()[0].quantity_, 1u);
    EXPECT_EQ(ob.get_order(1), resting);
}

TEST(Orderbook, FillOrKill_CanMatch)
{
    orderbook ob;
    auto ask =
        std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::sell, P(100.0), 100);
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
    ob.add_order(
        std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(99.0), 50));
    ob.add_order(
        std::make_shared<order>(ob_order_type::good_till_cancel, 2, side::sell, P(101.0), 30));
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
    ob.add_external_order(
        std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(50.0), 10));
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
    ob.add_order(
        std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, P(100.0), 100));
    ob.add_order(
        std::make_shared<order>(ob_order_type::good_till_cancel, 2, side::sell, P(101.0), 100));
    ob.clear();
    EXPECT_EQ(ob.size(), 0u);
}
