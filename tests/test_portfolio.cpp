#include <gtest/gtest.h>
#include "execution/portfolio.h"

static auto now() { return std::chrono::system_clock::now(); }

TEST(Portfolio, InitialState)
{
    portfolio p;
    EXPECT_FALSE(p.position_open());
    EXPECT_EQ(p.get_total_trades(), 0u);
}

TEST(Portfolio, BuyFill_OpensPosition)
{
    portfolio p;
    fill_event f(now(), "AAPL", 1, order_side::buy, 10, 100.0, 0.0);
    p.on_fill(f);
    EXPECT_TRUE(p.position_open());
    EXPECT_TRUE(p.position_open("AAPL"));
}

TEST(Portfolio, SellWithNoPosition_OpensShort)
{
    portfolio p;
    double initial_cash = p.get_cash();
    fill_event f(now(), "AAPL", 1, order_side::sell, 10, 100.0, 0.0);
    p.on_fill(f);
    EXPECT_TRUE(p.position_open());
    EXPECT_TRUE(p.position_open("AAPL"));
    // Short: cash increases by sale proceeds
    EXPECT_DOUBLE_EQ(p.get_cash(), initial_cash + 1000.0);
    // Equity at current price should equal initial (no profit yet)
    EXPECT_DOUBLE_EQ(p.get_equity(100.0), initial_cash);
    EXPECT_EQ(p.get_total_trades(), 0u);
}

TEST(Portfolio, ShortRoundTrip)
{
    portfolio p;
    double initial_cash = p.get_cash();
    // Open short at 100
    fill_event sell(now(), "AAPL", 1, order_side::sell, 10, 100.0, 0.0);
    p.on_fill(sell);
    // Close short at 90 (profit = 10 * 10 = 100)
    fill_event buy(now(), "AAPL", 2, order_side::buy, 10, 90.0, 0.0);
    p.on_fill(buy);
    EXPECT_FALSE(p.position_open());
    EXPECT_DOUBLE_EQ(p.get_cash(), initial_cash + 100.0);
    EXPECT_EQ(p.get_total_trades(), 1u);
}

TEST(Portfolio, BuySell_RoundTrip)
{
    portfolio p;
    fill_event buy(now(), "AAPL", 1, order_side::buy, 10, 100.0, 0.0);
    fill_event sell(now(), "AAPL", 2, order_side::sell, 10, 110.0, 0.0);
    p.on_fill(buy);
    p.on_fill(sell);
    EXPECT_FALSE(p.position_open());
    EXPECT_EQ(p.get_total_trades(), 1u);
}

TEST(Portfolio, PartialSell)
{
    portfolio p;
    fill_event buy(now(), "AAPL", 1, order_side::buy, 10, 100.0, 0.0);
    fill_event sell(now(), "AAPL", 2, order_side::sell, 5, 110.0, 0.0);
    p.on_fill(buy);
    p.on_fill(sell);
    EXPECT_TRUE(p.position_open());
    EXPECT_EQ(p.get_total_trades(), 0u); // not closed yet
}

TEST(Portfolio, PartialShortCoverReducesCostBasisTowardZero)
{
    portfolio p;
    const double initial_cash = p.get_cash();
    fill_event sell(now(), "AAPL", 1, order_side::sell, 10, 100.0, 5.0);
    fill_event buy(now(), "AAPL", 2, order_side::buy, 5, 90.0, 2.5);

    p.on_fill(sell);
    p.on_fill(buy);

    const auto& positions = p.get_positions();
    auto it = positions.find("AAPL");
    ASSERT_NE(it, positions.end());
    EXPECT_TRUE(p.position_open("AAPL"));
    EXPECT_DOUBLE_EQ(it->second.qty, -5.0);
    EXPECT_DOUBLE_EQ(it->second.cost_basis, -502.5);
    EXPECT_DOUBLE_EQ(p.get_cash(), initial_cash + 1000.0 - 5.0 - 450.0 - 2.5);
    EXPECT_EQ(p.get_total_trades(), 0u);
}

TEST(Portfolio, BuyFlipFromShortProratesCommissionOnce)
{
    portfolio p;
    const double initial_cash = p.get_cash();
    fill_event sell(now(), "AAPL", 1, order_side::sell, 5, 100.0, 5.0);
    fill_event buy(now(), "AAPL", 2, order_side::buy, 8, 90.0, 8.0);

    p.on_fill(sell);
    p.on_fill(buy);

    const auto& positions = p.get_positions();
    auto it = positions.find("AAPL");
    ASSERT_NE(it, positions.end());
    EXPECT_DOUBLE_EQ(it->second.qty, 3.0);
    EXPECT_DOUBLE_EQ(it->second.cost_basis, 273.0);
    EXPECT_DOUBLE_EQ(p.get_cash(), initial_cash + 500.0 - 5.0 - 450.0 - 5.0 - 270.0 - 3.0);
    EXPECT_EQ(p.get_total_trades(), 1u);
}

TEST(Portfolio, SellFlipFromLongProratesCommissionOnce)
{
    portfolio p;
    const double initial_cash = p.get_cash();
    fill_event buy(now(), "AAPL", 1, order_side::buy, 5, 100.0, 5.0);
    fill_event sell(now(), "AAPL", 2, order_side::sell, 8, 110.0, 8.0);

    p.on_fill(buy);
    p.on_fill(sell);

    const auto& positions = p.get_positions();
    auto it = positions.find("AAPL");
    ASSERT_NE(it, positions.end());
    EXPECT_DOUBLE_EQ(it->second.qty, -3.0);
    EXPECT_DOUBLE_EQ(it->second.cost_basis, -333.0);
    EXPECT_DOUBLE_EQ(p.get_cash(), initial_cash - 500.0 - 5.0 + 550.0 - 5.0 + 330.0 - 3.0);
    EXPECT_EQ(p.get_total_trades(), 1u);
}

TEST(Portfolio, MultipleBuys_AverageEntry)
{
    portfolio p;
    fill_event buy1(now(), "AAPL", 1, order_side::buy, 5, 100.0, 0.0);
    fill_event buy2(now(), "AAPL", 2, order_side::buy, 5, 120.0, 0.0);
    p.on_fill(buy1);
    p.on_fill(buy2);
    // avg entry = (5*100 + 5*120) / 10 = 110
    fill_event sell(now(), "AAPL", 3, order_side::sell, 10, 115.0, 0.0);
    p.on_fill(sell);
    EXPECT_FALSE(p.position_open());
    EXPECT_EQ(p.get_total_trades(), 1u);
}

TEST(Portfolio, BuyWithCommission)
{
    portfolio p;
    fill_event f(now(), "AAPL", 1, order_side::buy, 10, 100.0, 10.0);
    // total_cost for buy = 10*100 + 10 = 1010
    p.on_fill(f);
    EXPECT_TRUE(p.position_open());
}

TEST(Portfolio, SellWithCommission)
{
    portfolio p;
    fill_event buy(now(), "AAPL", 1, order_side::buy, 10, 100.0, 0.0);
    fill_event sell(now(), "AAPL", 2, order_side::sell, 10, 110.0, 5.0);
    p.on_fill(buy);
    p.on_fill(sell);
    EXPECT_FALSE(p.position_open());
    EXPECT_EQ(p.get_total_trades(), 1u);
}

// --- Multi-Symbol Tests ---

TEST(Portfolio, MultiSymbol_IndependentPositions)
{
    portfolio p;
    fill_event buy_aapl(now(), "AAPL", 1, order_side::buy, 10, 100.0, 0.0);
    fill_event buy_goog(now(), "GOOG", 2, order_side::buy, 5, 200.0, 0.0);
    p.on_fill(buy_aapl);
    p.on_fill(buy_goog);

    EXPECT_TRUE(p.position_open("AAPL"));
    EXPECT_TRUE(p.position_open("GOOG"));
    EXPECT_TRUE(p.position_open()); // any position open

    // Sell only AAPL
    fill_event sell_aapl(now(), "AAPL", 3, order_side::sell, 10, 110.0, 0.0);
    p.on_fill(sell_aapl);

    EXPECT_FALSE(p.position_open("AAPL"));
    EXPECT_TRUE(p.position_open("GOOG"));
    EXPECT_TRUE(p.position_open()); // GOOG still open
    EXPECT_EQ(p.get_total_trades(), 1u);
}

TEST(Portfolio, MultiSymbol_BothClosed)
{
    portfolio p;
    fill_event buy_a(now(), "A", 1, order_side::buy, 10, 50.0, 0.0);
    fill_event buy_b(now(), "B", 2, order_side::buy, 10, 60.0, 0.0);
    p.on_fill(buy_a);
    p.on_fill(buy_b);

    fill_event sell_a(now(), "A", 3, order_side::sell, 10, 55.0, 0.0);
    fill_event sell_b(now(), "B", 4, order_side::sell, 10, 65.0, 0.0);
    p.on_fill(sell_a);
    p.on_fill(sell_b);

    EXPECT_FALSE(p.position_open());
    EXPECT_EQ(p.get_total_trades(), 2u);
}

TEST(Portfolio, MultiSymbol_CashTracking)
{
    portfolio p;
    double initial_cash = p.get_cash();

    fill_event buy_a(now(), "A", 1, order_side::buy, 10, 100.0, 0.0);
    p.on_fill(buy_a);
    EXPECT_DOUBLE_EQ(p.get_cash(), initial_cash - 1000.0);

    fill_event buy_b(now(), "B", 2, order_side::buy, 5, 200.0, 0.0);
    p.on_fill(buy_b);
    EXPECT_DOUBLE_EQ(p.get_cash(), initial_cash - 1000.0 - 1000.0);
}
