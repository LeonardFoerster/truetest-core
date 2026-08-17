#include <gtest/gtest.h>
#include "execution/portfolio.h"

#include <cmath>
#include <limits>

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
    // Short open basis uses (notional - commission); half remains after 50% cover.
    EXPECT_DOUBLE_EQ(it->second.cost_basis, -497.5);
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
    // Flip residual short: 3 * 110 - prorated commission 3 = 327.
    EXPECT_DOUBLE_EQ(it->second.cost_basis, -327.0);
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

TEST(Portfolio, ShortPartialCover_CostBasisScalesProportionally)
{
    portfolio p;
    double initial_cash = p.get_cash();

    // Open short 10 @ 100 → cost_basis = -1000
    fill_event sell(now(), "AAPL", 1, order_side::sell, 10, 100.0, 0.0);
    p.on_fill(sell);

    // Cover 5 @ 90 → remaining short 5 keeps per-unit entry of 100
    fill_event buy(now(), "AAPL", 2, order_side::buy, 5, 90.0, 0.0);
    p.on_fill(buy);

    const auto& pos = p.get_positions().at("AAPL");
    EXPECT_NEAR(pos.qty, -5.0, 1e-12);
    EXPECT_NEAR(pos.cost_basis, -500.0, 1e-9);
    // Realized so far: 5 * (100 - 90) = 50; cash = initial + 1000 - 450
    EXPECT_NEAR(p.get_cash(), initial_cash + 550.0, 1e-9);
    // Equity at 90: unrealized 5 * (100 - 90) = 50 on top
    EXPECT_NEAR(p.get_equity(90.0), initial_cash + 100.0, 1e-9);
}

TEST(Portfolio, FlipLongToShort_CommissionChargedOnce)
{
    portfolio p;
    double initial_cash = p.get_cash();

    fill_event buy(now(), "AAPL", 1, order_side::buy, 10, 100.0, 0.0);
    p.on_fill(buy);

    // Sell 15 @ 100 with 15 commission: 10 close (comm 10) + 5 open (comm 5).
    fill_event flip(now(), "AAPL", 2, order_side::sell, 15, 100.0, 15.0);
    p.on_fill(flip);

    const auto& pos = p.get_positions().at("AAPL");
    EXPECT_NEAR(pos.qty, -5.0, 1e-12);
    // cash = initial - 1000 + (1000 - 10) + (500 - 5)
    EXPECT_NEAR(p.get_cash(), initial_cash + 485.0, 1e-9);
    // Short basis: -(5 * 100) + prorated open commission
    EXPECT_NEAR(pos.cost_basis, -495.0, 1e-9);
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

TEST(Portfolio, EconomicOverflowFillLeavesEveryLedgerFieldUnchanged)
{
    portfolio p(1000.0);
    fill_event open(now(), "SAFE", 1, order_side::buy, 2.0, 100.0, 1.0);
    p.on_fill(open);

    const double cash_before = p.get_cash();
    const auto position_before = p.get_positions().at("SAFE");
    const auto lot_before = p.get_lots().at(1);
    const auto fills_before = p.get_total_fills();
    const auto trades_before = p.get_total_trades();

    // All input fields are finite.  The unsafe product is not.
    fill_event overflow(now(), "SAFE", 2, order_side::buy,
                        std::numeric_limits<double>::max(), 2.0, 0.0);
    p.on_fill(overflow);

    EXPECT_DOUBLE_EQ(p.get_cash(), cash_before);
    ASSERT_EQ(p.get_positions().size(), 1u);
    EXPECT_DOUBLE_EQ(p.get_positions().at("SAFE").qty, position_before.qty);
    EXPECT_DOUBLE_EQ(p.get_positions().at("SAFE").cost_basis, position_before.cost_basis);
    ASSERT_EQ(p.get_lots().size(), 1u);
    EXPECT_DOUBLE_EQ(p.get_lots().at(1).qty_open, lot_before.qty_open);
    EXPECT_DOUBLE_EQ(p.get_lots().at(1).entry_price, lot_before.entry_price);
    EXPECT_DOUBLE_EQ(p.get_lots().at(1).entry_filled_qty, lot_before.entry_filled_qty);
    EXPECT_EQ(p.get_total_fills(), fills_before);
    EXPECT_EQ(p.get_total_trades(), trades_before);
    EXPECT_TRUE(std::isfinite(p.get_cash()));
}

TEST(Portfolio, CashQuantityBasisAndCommissionOverflowAreAllRejectedAtomically)
{
    const auto max = std::numeric_limits<double>::max();

    // Cash addition overflow on a short open.
    portfolio cash_port(max);
    const double cash_before = cash_port.get_cash();
    fill_event cash_overflow(now(), "CASH", 1, order_side::sell, 1.0, max, 0.0);
    cash_port.on_fill(cash_overflow);
    EXPECT_DOUBLE_EQ(cash_port.get_cash(), cash_before);
    EXPECT_TRUE(cash_port.get_positions().empty());

    // Position quantity overflow after one otherwise valid huge fill.
    portfolio qty_port;
    fill_event first_qty(now(), "QTY", 1, order_side::buy, max, 1.0, 0.0);
    qty_port.on_fill(first_qty);
    const auto qty_before = qty_port.get_positions().at("QTY");
    const auto fills_before = qty_port.get_total_fills();
    fill_event qty_overflow(now(), "QTY", 2, order_side::buy, max, 1.0, 0.0);
    qty_port.on_fill(qty_overflow);
    ASSERT_EQ(qty_port.get_positions().size(), 1u);
    EXPECT_DOUBLE_EQ(qty_port.get_positions().at("QTY").qty, qty_before.qty);
    EXPECT_DOUBLE_EQ(qty_port.get_positions().at("QTY").cost_basis, qty_before.cost_basis);
    EXPECT_EQ(qty_port.get_total_fills(), fills_before);

    // A finite notional plus a finite commission may still overflow.
    portfolio commission_port;
    const auto commission_cash_before = commission_port.get_cash();
    fill_event commission_overflow(now(), "FEE", 1, order_side::buy,
                                   1.0, max / 2.0, max);
    commission_port.on_fill(commission_overflow);
    EXPECT_DOUBLE_EQ(commission_port.get_cash(), commission_cash_before);
    EXPECT_TRUE(commission_port.get_positions().empty());

    // Cost basis has the same checked-add requirement.
    portfolio basis_port;
    fill_event basis_one(now(), "BASIS", 1, order_side::buy, 1.0, max * 0.75, 0.0);
    basis_port.on_fill(basis_one);
    const auto basis_before = basis_port.get_positions().at("BASIS");
    fill_event basis_overflow(now(), "BASIS", 2, order_side::buy, 1.0, max * 0.75, 0.0);
    basis_port.on_fill(basis_overflow);
    ASSERT_EQ(basis_port.get_positions().size(), 1u);
    EXPECT_DOUBLE_EQ(basis_port.get_positions().at("BASIS").qty, basis_before.qty);
    EXPECT_DOUBLE_EQ(basis_port.get_positions().at("BASIS").cost_basis, basis_before.cost_basis);
}
