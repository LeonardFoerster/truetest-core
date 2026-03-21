#include <gtest/gtest.h>
#include "orderbook/orderbook_registry.h"

TEST(OrderbookRegistry, GetOrCreate_NewSymbol)
{
    OrderbookRegistry reg;
    auto ob = reg.get_or_create("AAPL");
    ASSERT_NE(ob, nullptr);
    EXPECT_EQ(reg.size(), 1u);
}

TEST(OrderbookRegistry, GetOrCreate_SameSymbol)
{
    OrderbookRegistry reg;
    auto ob1 = reg.get_or_create("AAPL");
    auto ob2 = reg.get_or_create("AAPL");
    EXPECT_EQ(ob1, ob2);
    EXPECT_EQ(reg.size(), 1u);
}

TEST(OrderbookRegistry, GetOrCreate_MultipleSymbols)
{
    OrderbookRegistry reg;
    auto ob1 = reg.get_or_create("AAPL");
    auto ob2 = reg.get_or_create("GOOG");
    EXPECT_NE(ob1, ob2);
    EXPECT_EQ(reg.size(), 2u);
}

TEST(OrderbookRegistry, Get_Existing)
{
    OrderbookRegistry reg;
    reg.get_or_create("AAPL");
    auto ob = reg.get("AAPL");
    EXPECT_NE(ob, nullptr);
}

TEST(OrderbookRegistry, Get_NonExisting)
{
    OrderbookRegistry reg;
    auto ob = reg.get("AAPL");
    EXPECT_EQ(ob, nullptr);
}

TEST(OrderbookRegistry, Symbols)
{
    OrderbookRegistry reg;
    reg.get_or_create("AAPL");
    reg.get_or_create("GOOG");
    reg.get_or_create("MSFT");
    auto syms = reg.symbols();
    EXPECT_EQ(syms.size(), 3u);
}

TEST(OrderbookRegistry, Clear)
{
    OrderbookRegistry reg;
    reg.get_or_create("AAPL");
    reg.get_or_create("GOOG");
    reg.clear();
    EXPECT_EQ(reg.size(), 0u);
    EXPECT_EQ(reg.get("AAPL"), nullptr);
}

TEST(OrderbookRegistry, IndependentBooks)
{
    OrderbookRegistry reg;
    auto ob_aapl = reg.get_or_create("AAPL");
    auto ob_goog = reg.get_or_create("GOOG");

    // Add order to AAPL book only
    auto o = std::make_shared<order>(ob_order_type::good_till_cancel, 1, side::buy, Price::from_double(100.0), 100);
    ob_aapl->add_order(o);

    EXPECT_EQ(ob_aapl->size(), 1u);
    EXPECT_EQ(ob_goog->size(), 0u);
}
