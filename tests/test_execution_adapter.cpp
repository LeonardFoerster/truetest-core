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

TEST(ExchangeAdapter, Stub)
{
    ExchangeAdapter adapter;
    order_event o(now(), "TEST", order_type::market, order_side::buy, 1, 100.0);
    adapter.submit_order(o); // no crash

    std::vector<fill_event> fills;
    EXPECT_FALSE(adapter.poll_fills(fills));
}
