#include <gtest/gtest.h>
#include "execution/execution_adapter.h"

static auto now() { return std::chrono::system_clock::now(); }

TEST(LocalBookAdapter, SubmitAndFill)
{
    auto ob = std::make_shared<orderbook>();
    // Seed an ask at price 100 (10000 in fixed-point)
    ob->add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 1000, side::sell, Price::from_double(100.0), 100));

    LocalBookAdapter adapter(ob, nullptr, nullptr);

    order_event o(now(), "TEST", order_type::limit, order_side::buy, 100, 100.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    EXPECT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_fill_price(), 100.0, 0.01);
}

TEST(LocalBookAdapter, NoMatch)
{
    auto ob = std::make_shared<orderbook>();
    LocalBookAdapter adapter(ob, nullptr, nullptr);

    order_event o(now(), "TEST", order_type::limit, order_side::buy, 100, 90.0);
    o.set_order_id(1);
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    EXPECT_FALSE(adapter.poll_fills(fills));
}

TEST(LocalBookAdapter, WithFeeModel)
{
    auto ob = std::make_shared<orderbook>();
    ob->add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 1000, side::sell, Price::from_double(100.0), 100));

    auto fee = std::make_shared<FixedFeeModel>(5.0);
    LocalBookAdapter adapter(ob, fee, nullptr);

    order_event o(now(), "TEST", order_type::limit, order_side::buy, 100, 100.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    EXPECT_DOUBLE_EQ(fills[0].get_commission(), 5.0);
}

TEST(LocalBookAdapter, PerfectFillModel)
{
    auto ob = std::make_shared<orderbook>();
    ob->add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 1000, side::sell, Price::from_double(100.0), 100));

    auto fill_model = std::make_shared<PerfectFillModel>();
    LocalBookAdapter adapter(ob, nullptr, fill_model);
    adapter.set_mid_price(100.0);

    order_event o(now(), "TEST", order_type::limit, order_side::buy, 100, 100.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    EXPECT_EQ(fills.size(), 1u);
}

TEST(LocalBookAdapter, PollClears)
{
    auto ob = std::make_shared<orderbook>();
    ob->add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 1000, side::sell, Price::from_double(100.0), 100));

    LocalBookAdapter adapter(ob, nullptr, nullptr);

    order_event o(now(), "TEST", order_type::limit, order_side::buy, 100, 100.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));

    std::vector<fill_event> fills2;
    EXPECT_FALSE(adapter.poll_fills(fills2));
}

TEST(LocalBookAdapter, SyntheticFillProvenanceCapturesModelAndWalkedDepth)
{
    const auto decision_ts = std::chrono::system_clock::time_point(std::chrono::milliseconds(10));
    const auto eligible_ts = std::chrono::system_clock::time_point(std::chrono::milliseconds(15));
    auto ob = std::make_shared<orderbook>();
    ob->add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 1000, side::sell,
        Price::from_double(100.0), 100000000));
    ob->add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 1001, side::sell,
        Price::from_double(101.0), 200000000));

    LocalBookAdapter adapter(ob, std::make_shared<FixedFeeModel>(0.25), nullptr,
                             /*rng_seed=*/42, /*market_aggression=*/1.1,
                             /*qty_scale=*/1e8, /*latency_model=*/nullptr,
                             std::make_shared<SquareRootImpactModel>(10.0, 3.0));
    adapter.set_mid_price(100.0);

    order_event order(decision_ts, "TEST", order_type::market,
                      order_side::buy, 3.0, 100.0);
    order.set_order_id(7);
    order.set_decision_ts(decision_ts);
    order.set_earliest_eligible_ts(eligible_ts);
    adapter.submit_order(order);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 2u);

    EXPECT_NEAR(fills[0].get_filled_quantity(), 1.0, 1e-9);
    EXPECT_NEAR(fills[0].get_remaining_qty(), 2.0, 1e-9);
    EXPECT_NEAR(fills[1].get_filled_quantity(), 2.0, 1e-9);
    EXPECT_NEAR(fills[1].get_remaining_qty(), 0.0, 1e-9);
    EXPECT_NE(fills[0].get_fill_id(), 0u);
    EXPECT_NE(fills[1].get_fill_id(), 0u);
    EXPECT_NE(fills[0].get_fill_id(), fills[1].get_fill_id());
    EXPECT_EQ(fills[0].get_source(), fill_source::simulated);
    EXPECT_DOUBLE_EQ(fills[0].get_commission(), 0.25);
    EXPECT_DOUBLE_EQ(fills[1].get_commission(), 0.25);

    for (const auto& fill : fills)
    {
        const auto& provenance = fill.get_provenance();
        EXPECT_EQ(provenance.model, fill_execution_model::synthetic_local_liquidity);
        EXPECT_EQ(provenance.reason, fill_execution_reason::aggressive_ladder_match);
        EXPECT_TRUE(provenance.exploratory);
        EXPECT_DOUBLE_EQ(provenance.intended_price, 100.0);
        EXPECT_NEAR(provenance.reference_price, 100.1, 1e-9);
        EXPECT_EQ(provenance.reference_timestamp, eligible_ts);
        EXPECT_DOUBLE_EQ(provenance.fill_probability, 1.0);
        EXPECT_EQ(provenance.modeled_latency.count(), 5000000);
        EXPECT_NEAR(provenance.modeled_impact_bps, 10.0, 1e-9);
    }
    EXPECT_LT(fills[0].get_provenance().modeled_spread_bps, 0.0);
    EXPECT_GT(fills[1].get_provenance().modeled_spread_bps, 0.0);
}

TEST(LocalBookAdapter, BarRangeFillCarriesExploratoryReference)
{
    const auto submit_ts = std::chrono::system_clock::time_point(std::chrono::milliseconds(10));
    const auto bar_ts = std::chrono::system_clock::time_point(std::chrono::milliseconds(20));
    auto ob = std::make_shared<orderbook>();
    LocalBookAdapter adapter(ob, nullptr, nullptr);

    order_event order(submit_ts, "TEST", order_type::limit,
                      order_side::buy, 2.0, 99.0);
    order.set_order_id(8);
    order.set_decision_ts(submit_ts);
    order.set_earliest_eligible_ts(submit_ts);
    adapter.submit_order(order);

    ASSERT_TRUE(adapter.sweep_resting_range("TEST", 98.0, 101.0, bar_ts));
    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);

    const auto& provenance = fills.front().get_provenance();
    EXPECT_EQ(fills.front().get_source(), fill_source::simulated);
    EXPECT_EQ(provenance.model, fill_execution_model::synthetic_local_liquidity);
    EXPECT_EQ(provenance.reason, fill_execution_reason::bar_range_sweep);
    EXPECT_TRUE(provenance.exploratory);
    EXPECT_DOUBLE_EQ(provenance.intended_price, 99.0);
    EXPECT_DOUBLE_EQ(provenance.reference_price, 99.0);
    EXPECT_EQ(provenance.reference_timestamp, bar_ts);
}

TEST(ExchangeAdapter, Stub)
{
    ExchangeAdapter adapter;
    order_event o(now(), "TEST", order_type::market, order_side::buy, 1, 100.0);
    adapter.submit_order(o); // no crash

    std::vector<fill_event> fills;
    EXPECT_FALSE(adapter.poll_fills(fills));
}
