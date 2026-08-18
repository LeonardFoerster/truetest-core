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

TEST(Portfolio, BF13_LongToShortFlipSplitsCloseAndNewOpenerLot)
{
    portfolio p;
    const double initial_cash = p.get_cash();
    const auto ts = now();

    fill_event open(ts, "AAPL", 101, order_side::buy,
                    10.0, 100.0, 10.0,
                    /*remaining_qty=*/0.0, /*fill_id=*/1,
                    "alpha", /*opener_order_id=*/101);
    p.on_fill(open, 101, "alpha");

    // One physical fill closes the old 10-unit long and opens a new
    // 5-unit short.  Its 15 commission is prorated 10/5 by net accounting.
    fill_event flip(ts, "AAPL", 202, order_side::sell,
                    15.0, 120.0, 15.0,
                    /*remaining_qty=*/0.0, /*fill_id=*/2,
                    "alpha", /*opener_order_id=*/101);
    p.on_fill(flip, 101, "alpha");

    const auto& pos = p.get_positions().at("AAPL");
    EXPECT_DOUBLE_EQ(pos.qty, -5.0);
    EXPECT_DOUBLE_EQ(pos.cost_basis, -595.0);
    EXPECT_EQ(p.get_total_fills(), 2u);
    EXPECT_EQ(p.get_total_trades(), 1u);

    // Realized old-leg PnL is 200 gross - 10 entry fee - 10 close fee;
    // the residual opener's 5 fee remains reflected in marked equity.
    EXPECT_DOUBLE_EQ(p.get_cash(), initial_cash - 1010.0 + 1190.0 + 595.0);
    EXPECT_DOUBLE_EQ(p.get_equity(120.0), initial_cash + 175.0);

    const auto& lots = p.get_lots();
    EXPECT_EQ(lots.count(101), 0u);
    ASSERT_EQ(lots.size(), 1u);
    auto residual = lots.find(202);
    ASSERT_NE(residual, lots.end());
    EXPECT_EQ(residual->second.side, order_side::sell);
    EXPECT_DOUBLE_EQ(residual->second.qty_open, 5.0);
    EXPECT_DOUBLE_EQ(residual->second.entry_filled_qty, 5.0);
    EXPECT_DOUBLE_EQ(residual->second.entry_price, 120.0);
    EXPECT_EQ(residual->second.strategy_name, "alpha");
}

TEST(Portfolio, BF13_ShortToLongFlipSplitsCloseAndNewOpenerLot)
{
    portfolio p;
    const double initial_cash = p.get_cash();
    const auto ts = now();

    fill_event open(ts, "AAPL", 301, order_side::sell,
                    10.0, 100.0, 10.0,
                    /*remaining_qty=*/0.0, /*fill_id=*/1,
                    "alpha", /*opener_order_id=*/301);
    p.on_fill(open, 301, "alpha");

    fill_event flip(ts, "AAPL", 402, order_side::buy,
                    15.0, 80.0, 15.0,
                    /*remaining_qty=*/0.0, /*fill_id=*/2,
                    "alpha", /*opener_order_id=*/301);
    p.on_fill(flip, 301, "alpha");

    const auto& pos = p.get_positions().at("AAPL");
    EXPECT_DOUBLE_EQ(pos.qty, 5.0);
    EXPECT_DOUBLE_EQ(pos.cost_basis, 405.0);
    EXPECT_EQ(p.get_total_fills(), 2u);
    EXPECT_EQ(p.get_total_trades(), 1u);
    EXPECT_DOUBLE_EQ(p.get_cash(), initial_cash + 990.0 - 810.0 - 405.0);
    EXPECT_DOUBLE_EQ(p.get_equity(80.0), initial_cash + 175.0);

    const auto& lots = p.get_lots();
    EXPECT_EQ(lots.count(301), 0u);
    ASSERT_EQ(lots.size(), 1u);
    auto residual = lots.find(402);
    ASSERT_NE(residual, lots.end());
    EXPECT_EQ(residual->second.side, order_side::buy);
    EXPECT_DOUBLE_EQ(residual->second.qty_open, 5.0);
    EXPECT_DOUBLE_EQ(residual->second.entry_filled_qty, 5.0);
    EXPECT_DOUBLE_EQ(residual->second.entry_price, 80.0);
    EXPECT_EQ(residual->second.strategy_name, "alpha");
}

TEST(Portfolio, BF17_FlipOnlyConsumesReferencedLotAndOpensResidual)
{
    portfolio p;
    const auto ts = now();

    fill_event first(ts, "AAPL", 11, order_side::buy, 10.0, 100.0, 0.0);
    fill_event second(ts, "AAPL", 12, order_side::buy, 7.0, 105.0, 0.0);
    p.on_fill(first, 11, "alpha");
    p.on_fill(second, 12, "beta");

    // The fill is explicitly attributed to lot 11. It closes only that lot;
    // the five-unit overshoot becomes a new short lot even though lot 12
    // keeps the venue's aggregate net position long.
    fill_event flip(ts, "AAPL", 22, order_side::sell, 15.0, 110.0, 0.0);
    p.on_fill(flip, 11, "alpha");

    const auto& lots = p.get_lots();
    EXPECT_EQ(lots.count(11), 0u);
    ASSERT_NE(lots.find(12), lots.end());
    EXPECT_DOUBLE_EQ(lots.at(12).qty_open, 7.0);
    const auto residual = lots.find(22);
    ASSERT_NE(residual, lots.end());
    EXPECT_EQ(residual->second.side, order_side::sell);
    EXPECT_DOUBLE_EQ(residual->second.qty_open, 5.0);
    EXPECT_DOUBLE_EQ(residual->second.entry_filled_qty, 5.0);

    const auto& pos = p.get_positions().at("AAPL");
    EXPECT_DOUBLE_EQ(pos.qty, 2.0);
}

TEST(Portfolio, FlipAcrossPartialFillsContinuesNewOpenerLot)
{
    portfolio p;
    const auto ts = now();

    fill_event open(ts, "AAPL", 51, order_side::buy, 10.0, 100.0, 0.0);
    p.on_fill(open, 51, "alpha");

    // The first physical partial fill consumes the old lot exactly, while
    // the order still has five units left to fill.
    fill_event first_flip(ts, "AAPL", 52, order_side::sell,
                          10.0, 110.0, 0.0,
                          /*remaining_qty=*/5.0);
    p.on_fill(first_flip, 51, "alpha");

    // The old opener no longer exists. Subsequent fills are entirely opener
    // fills of the same order and roll into one weighted residual lot.
    fill_event second_flip(ts, "AAPL", 52, order_side::sell,
                           2.0, 115.0, 0.0,
                           /*remaining_qty=*/3.0);
    p.on_fill(second_flip, 51, "alpha");
    fill_event third_flip(ts, "AAPL", 52, order_side::sell,
                           3.0, 120.0, 0.0,
                           /*remaining_qty=*/0.0);
    p.on_fill(third_flip, 51, "alpha");

    const auto& lots = p.get_lots();
    EXPECT_EQ(lots.count(51), 0u);
    ASSERT_EQ(lots.size(), 1u);
    const auto residual = lots.find(52);
    ASSERT_NE(residual, lots.end());
    EXPECT_DOUBLE_EQ(residual->second.qty_open, 5.0);
    EXPECT_DOUBLE_EQ(residual->second.entry_filled_qty, 5.0);
    EXPECT_DOUBLE_EQ(residual->second.entry_price, 118.0);

    const auto& pos = p.get_positions().at("AAPL");
    EXPECT_DOUBLE_EQ(pos.qty, -5.0);
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
