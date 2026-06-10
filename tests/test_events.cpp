#include <gtest/gtest.h>
#include "core/event.h"

static auto now() { return std::chrono::system_clock::now(); }

TEST(MarketEvent, Construction)
{
    auto ts = now();
    market_event e(ts, "AAPL", 100.0, 110.0, 90.0, 105.0, 5000);

    EXPECT_EQ(e.get_type(), event_type::market);
    EXPECT_EQ(e.get_symbol(), "AAPL");
    EXPECT_DOUBLE_EQ(e.get_open(), 100.0);
    EXPECT_DOUBLE_EQ(e.get_high(), 110.0);
    EXPECT_DOUBLE_EQ(e.get_low(), 90.0);
    EXPECT_DOUBLE_EQ(e.get_close(), 105.0);
    EXPECT_EQ(e.get_volume(), 5000);
    EXPECT_EQ(e.get_timestamp(), ts);
}

TEST(MarketEvent, ToString)
{
    auto s = market_event(now(), "BTC", 1.0, 2.0, 0.5, 1.5).to_string();
    EXPECT_NE(s.find("BTC"), std::string::npos);
}

TEST(OrderEvent, DefaultState)
{
    auto ts = now();
    order_event o(ts, "AAPL", order_type::limit, order_side::buy, 10, 100.0);
    EXPECT_EQ(o.get_order_id(), 0u);
    EXPECT_EQ(o.get_earliest_eligible_ts(), ts);
}

TEST(OrderEvent, SetOrderId)
{
    order_event o(now(), "AAPL", order_type::market, order_side::buy, 1, 0.0);
    o.set_order_id(42);
    EXPECT_EQ(o.get_order_id(), 42u);
}

TEST(OrderEvent, EarliestEligibleTs)
{
    auto ts = now();
    auto future = ts + std::chrono::milliseconds(500);
    order_event o(ts, "AAPL", order_type::market, order_side::buy, 1);
    o.set_earliest_eligible_ts(future);
    EXPECT_EQ(o.get_earliest_eligible_ts(), future);
}

TEST(FillEvent, TotalCost_Buy)
{
    fill_event f(now(), "AAPL", 1, order_side::buy, 10, 100.0, 5.0);
    EXPECT_DOUBLE_EQ(f.get_total_cost(), 1005.0);
}

TEST(FillEvent, TotalCost_Sell)
{
    fill_event f(now(), "AAPL", 1, order_side::sell, 10, 100.0, 5.0);
    EXPECT_DOUBLE_EQ(f.get_total_cost(), 995.0);
}

TEST(FillEvent, ZeroCommission)
{
    fill_event f(now(), "AAPL", 1, order_side::buy, 10, 100.0, 0.0);
    EXPECT_DOUBLE_EQ(f.get_total_cost(), 1000.0);
}

TEST(TickEvent, Construction)
{
    auto ts = now();
    tick_event e(ts, "BTC", 50000.0, 1, tick_side::bid);
    EXPECT_EQ(e.get_type(), event_type::tick);
    EXPECT_EQ(e.get_symbol(), "BTC");
    EXPECT_DOUBLE_EQ(e.get_price(), 50000.0);
    EXPECT_EQ(e.get_quantity(), 1);
    EXPECT_EQ(e.get_side(), tick_side::bid);
}

TEST(TickEvent, DefaultSide)
{
    tick_event e(now(), "X", 1.0, 1);
    EXPECT_EQ(e.get_side(), tick_side::unknown);
}

TEST(L2SnapshotEvent, Construction)
{
    std::vector<l2_level> bids = {{100.0, 10}, {99.0, 20}, {98.0, 30}};
    std::vector<l2_level> asks = {{101.0, 15}, {102.0, 25}};
    l2_snapshot_event e(now(), "ETH",
                        bids.data(), bids.size(),
                        asks.data(), asks.size());

    EXPECT_EQ(e.get_type(), event_type::l2_snapshot);
    EXPECT_EQ(e.bid_count(), 3u);
    EXPECT_EQ(e.ask_count(), 2u);
    EXPECT_DOUBLE_EQ(e.bid(0).price, 100.0);
    EXPECT_EQ(e.ask(1).quantity, 25);
}

TEST(L2UpdateEvent, Construction)
{
    l2_update_event e(now(), "ETH", tick_side::bid, 100.0, 50);
    EXPECT_EQ(e.get_type(), event_type::l2_update);
    EXPECT_EQ(e.get_side(), tick_side::bid);
    EXPECT_DOUBLE_EQ(e.get_price(), 100.0);
    EXPECT_EQ(e.get_new_quantity(), 50);
}

TEST(Event, Polymorphism)
{
    event_pointer m = std::make_shared<market_event>(now(), "A", 1, 2, 0, 1);
    event_pointer o = std::make_shared<order_event>(now(), "A", order_type::market, order_side::buy, 1);
    event_pointer f = std::make_shared<fill_event>(now(), "A", 1, order_side::buy, 1, 1.0);
    event_pointer t = std::make_shared<tick_event>(now(), "A", 1.0, 1);

    EXPECT_EQ(m->get_type(), event_type::market);
    EXPECT_EQ(o->get_type(), event_type::order);
    EXPECT_EQ(f->get_type(), event_type::fill);
    EXPECT_EQ(t->get_type(), event_type::tick);
}
