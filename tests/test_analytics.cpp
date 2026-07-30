#include <gtest/gtest.h>
#include "analytics/analytics.h"

static auto epoch_ms(int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

TEST(Analytics, InitialReport)
{
    Analytics a;
    auto r = a.generate_report();
    EXPECT_DOUBLE_EQ(r.initial_equity, 100000.0);
    EXPECT_DOUBLE_EQ(r.final_equity, 100000.0);
    EXPECT_EQ(r.total_trades, 0u);
    EXPECT_EQ(r.total_orders, 0u);
    EXPECT_EQ(r.total_fills, 0u);
}

TEST(Analytics, EquityTail_EmptyBeforeAnyEvents)
{
    Analytics a;
    EXPECT_TRUE(a.equity_tail(10).empty());
    EXPECT_TRUE(a.drawdown_tail(10).empty());
}

TEST(Analytics, EquityTail_ReturnsLastNValuesInOrder)
{
    Analytics a;
    for (int i = 0; i < 5; ++i)
    {
        auto m = std::make_shared<market_event>(epoch_ms(i), "X",
                                                100.0 + i, 100.0 + i,
                                                100.0 + i, 100.0 + i);
        a.on_event(m);
    }
    auto tail = a.equity_tail(3);
    ASSERT_EQ(tail.size(), 3u);
    // Equity is constant (no positions) -> 100000 across the tail.
    for (double v : tail) EXPECT_DOUBLE_EQ(v, 100000.0);

    // Asking for more than available returns all of it.
    EXPECT_EQ(a.equity_tail(99).size(), 5u);
    EXPECT_EQ(a.equity_tail(0).size(), 0u);
}

TEST(Analytics, DrawdownTail_ZeroAtPeakPositiveBelowPeak)
{
    Analytics a;
    auto m1 = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(m1);
    auto buy = std::make_shared<order_event>(epoch_ms(1), "X",
                  order_type::limit, order_side::buy, 100.0, 100.0);
    buy->set_order_id(1);
    a.on_event(buy);
    auto fill = std::make_shared<fill_event>(epoch_ms(1), "X", 1,
                  order_side::buy, 100.0, 100.0, 0.0);
    a.on_event(fill);

    // Price rises to 110 -> equity peak at ~101000.
    auto m2 = std::make_shared<market_event>(epoch_ms(2), "X", 110, 110, 110, 110.0);
    a.on_event(m2);
    // Then drops to 105 -> drawdown vs peak.
    auto m3 = std::make_shared<market_event>(epoch_ms(3), "X", 105, 105, 105, 105.0);
    a.on_event(m3);

    auto dd = a.drawdown_tail(4);
    ASSERT_GE(dd.size(), 2u);
    for (double v : dd) EXPECT_GE(v, 0.0);  // never negative
    EXPECT_GT(dd.back(), 0.0);              // last point is below peak
}

TEST(Analytics, MarketEvent_TracksPrice)
{
    Analytics a;
    auto m1 = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    auto m2 = std::make_shared<market_event>(epoch_ms(1), "X", 100, 100, 100, 110.0);
    auto m3 = std::make_shared<market_event>(epoch_ms(2), "X", 100, 100, 100, 120.0);
    a.on_event(m1);
    a.on_event(m2);
    a.on_event(m3);

    auto r = a.generate_report();
    EXPECT_EQ(r.equity_curve.size(), 3u);
    // Buy-and-hold: (120 - 100) / 100 = 0.2
    EXPECT_NEAR(r.buy_and_hold_return, 0.2, 0.001);
}

TEST(Analytics, OrderEvent_RecordsIntendedPrice)
{
    Analytics a;
    auto mkt = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt);

    auto ord = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 10, 100.0);
    ord->set_order_id(1);
    a.on_event(ord);

    auto r = a.generate_report();
    EXPECT_EQ(r.total_orders, 1u);
}

TEST(Analytics, FillEvent_BuyUpdatesEquity)
{
    Analytics a;
    auto mkt = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt);

    auto ord = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 10, 100.0);
    ord->set_order_id(1);
    a.on_event(ord);

    auto fill = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 10, 100.0, 0.0);
    a.on_event(fill);

    auto mkt2 = std::make_shared<market_event>(epoch_ms(2), "X", 110, 110, 110, 110.0);
    a.on_event(mkt2);

    auto r = a.generate_report();
    // In position: equity = cash + position * last_close
    // cash = 100000 - 1000 = 99000, position = 10 * 110 = 1100, total = 100100
    EXPECT_GT(r.equity_curve.back().equity, 100000.0);
}

TEST(Analytics, RoundTrip_PnL)
{
    Analytics a;
    auto mkt1 = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt1);

    auto buy_ord = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 10, 100.0);
    buy_ord->set_order_id(1);
    a.on_event(buy_ord);

    auto buy_fill = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 10, 100.0, 0.0);
    a.on_event(buy_fill);

    auto mkt2 = std::make_shared<market_event>(epoch_ms(2), "X", 110, 110, 110, 110.0);
    a.on_event(mkt2);

    auto sell_ord = std::make_shared<order_event>(epoch_ms(3), "X", order_type::limit, order_side::sell, 10, 110.0);
    sell_ord->set_order_id(2);
    a.on_event(sell_ord);

    auto sell_fill = std::make_shared<fill_event>(epoch_ms(3), "X", 2, order_side::sell, 10, 110.0, 0.0);
    a.on_event(sell_fill);

    auto r = a.generate_report();
    EXPECT_EQ(r.total_trades, 1u);
    EXPECT_GT(r.trade_returns[0], 0.0); // profitable trade
}

TEST(Analytics, WinRate)
{
    Analytics a;
    auto mkt = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt);

    uint64_t oid = 1;
    auto round_trip = [&](double entry, double exit, int64_t t_start) {
        auto bo = std::make_shared<order_event>(epoch_ms(t_start), "X", order_type::limit, order_side::buy, 10, entry);
        bo->set_order_id(oid);
        a.on_event(bo);
        auto bf = std::make_shared<fill_event>(epoch_ms(t_start), "X", oid, order_side::buy, 10, entry, 0.0);
        a.on_event(bf);
        oid++;
        auto so = std::make_shared<order_event>(epoch_ms(t_start + 1), "X", order_type::limit, order_side::sell, 10, exit);
        so->set_order_id(oid);
        a.on_event(so);
        auto sf = std::make_shared<fill_event>(epoch_ms(t_start + 1), "X", oid, order_side::sell, 10, exit, 0.0);
        a.on_event(sf);
        oid++;
    };

    round_trip(100.0, 110.0, 10);  // win
    round_trip(100.0, 120.0, 20);  // win
    round_trip(100.0, 130.0, 30);  // win
    round_trip(100.0, 90.0, 40);   // loss

    auto r = a.generate_report();
    EXPECT_EQ(r.total_trades, 4u);
    EXPECT_NEAR(r.win_rate, 75.0, 1.0);
}

TEST(Analytics, EventDispatch_UnknownType)
{
    Analytics a;
    auto tick = std::make_shared<tick_event>(epoch_ms(0), "X", 100.0, 1);
    auto l2 = std::make_shared<l2_update_event>(epoch_ms(0), "X", tick_side::bid, 100.0, 50);
    // Should not crash
    a.on_event(tick);
    a.on_event(l2);
    auto r = a.generate_report();
    EXPECT_EQ(r.total_fills, 0u);
}

// --- Step 7: Streaming / Incremental Analytics tests ---

TEST(Analytics, Sharpe_BarReturns_Annualized)
{
    // Sharpe is computed from bar-over-bar equity returns (not trade P&L) and
    // annualized by sqrt(periods_per_year). Feed a flat cash position (no
    // trades) and drive equity via mark-to-market of a pre-existing position,
    // then verify against a batch calculation with the same annualization.
    const std::size_t ppy = 252;
    Analytics a(100000.0, 252, 0.0, ppy);

    // Seed a long position so equity tracks price moves: buy 100 @ 100.
    auto mkt0 = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt0);
    auto bo = std::make_shared<order_event>(epoch_ms(0), "X", order_type::limit, order_side::buy, 100, 100.0);
    bo->set_order_id(1);
    a.on_event(bo);
    auto bf = std::make_shared<fill_event>(epoch_ms(0), "X", 1, order_side::buy, 100, 100.0, 0.0);
    a.on_event(bf);
    // Now cash = 100000 - 10000 = 90000, pos = 100.

    // Walk the price; each market_event produces a bar-over-bar equity return.
    const std::vector<double> prices = {102.0, 101.0, 105.0, 103.0, 108.0, 106.0, 110.0, 107.0};

    // Build expected equity curve and bar-over-bar returns.
    std::vector<double> equities;
    equities.push_back(90000.0 + 100.0 * 100.0); // prev_equity after mkt0 = 100000
    for (double p : prices) equities.push_back(90000.0 + 100.0 * p);

    std::vector<double> expected_rets;
    for (std::size_t i = 1; i < equities.size(); ++i)
        expected_rets.push_back((equities[i] - equities[i - 1]) / equities[i - 1]);

    int64_t t = 1;
    for (double p : prices)
    {
        auto m = std::make_shared<market_event>(epoch_ms(t++), "X", p, p, p, p);
        a.on_event(m);
    }

    double mean = 0.0;
    for (double r : expected_rets) mean += r;
    mean /= static_cast<double>(expected_rets.size());
    double sq = 0.0;
    for (double r : expected_rets) sq += (r - mean) * (r - mean);
    double stddev = std::sqrt(sq / static_cast<double>(expected_rets.size() - 1));
    double expected_sharpe = (stddev > 0.0)
        ? (mean / stddev) * std::sqrt(static_cast<double>(ppy))
        : 0.0;

    auto report = a.generate_report();
    EXPECT_NEAR(report.sharpe_ratio, expected_sharpe, 1e-9);
    EXPECT_GT(report.sharpe_ratio, 0.0);
}

TEST(Analytics, AnnualizedReturn_MatchesCompoundingFormula)
{
    // Eight daily bars with ppy=252 => annualization exponent 252/8 = 31.5
    const std::size_t ppy = 252;
    Analytics a(100000.0, 252, 0.0, ppy);

    auto mkt0 = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt0);
    auto bo = std::make_shared<order_event>(epoch_ms(0), "X", order_type::limit, order_side::buy, 100, 100.0);
    bo->set_order_id(1);
    a.on_event(bo);
    auto bf = std::make_shared<fill_event>(epoch_ms(0), "X", 1, order_side::buy, 100, 100.0, 0.0);
    a.on_event(bf);

    const std::vector<double> prices = {101.0, 102.0, 103.0, 104.0, 105.0, 106.0, 107.0, 108.0};
    int64_t t = 1;
    for (double p : prices)
    {
        auto m = std::make_shared<market_event>(epoch_ms(t++), "X", p, p, p, p);
        a.on_event(m);
    }

    auto r = a.generate_report();
    double expected_ann = std::pow(1.0 + r.cumulative_return,
                                    static_cast<double>(ppy) / static_cast<double>(prices.size())) - 1.0;
    EXPECT_NEAR(r.annualized_return, expected_ann, 1e-12);
}

TEST(Analytics, RunningDrawdown_MatchesPostHoc)
{
    Analytics a(50000.0);

    // Simulate equity curve: 50000 -> 55000 -> 52000 -> 58000 -> 51000
    // Max drawdown should be (58000 - 51000) / 58000
    auto mkt1 = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt1);

    // Buy 100 shares at 100
    auto bo = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 100, 100.0);
    bo->set_order_id(1);
    a.on_event(bo);
    auto bf = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 100, 100.0, 0.0);
    a.on_event(bf);
    // cash = 50000 - 10000 = 40000, position = 100 shares

    // Market at 150: equity = 40000 + 100*150 = 55000
    auto mkt2 = std::make_shared<market_event>(epoch_ms(2), "X", 150, 150, 150, 150.0);
    a.on_event(mkt2);

    // Market at 120: equity = 40000 + 100*120 = 52000
    auto mkt3 = std::make_shared<market_event>(epoch_ms(3), "X", 120, 120, 120, 120.0);
    a.on_event(mkt3);

    // Market at 180: equity = 40000 + 100*180 = 58000
    auto mkt4 = std::make_shared<market_event>(epoch_ms(4), "X", 180, 180, 180, 180.0);
    a.on_event(mkt4);

    // Market at 110: equity = 40000 + 100*110 = 51000
    auto mkt5 = std::make_shared<market_event>(epoch_ms(5), "X", 110, 110, 110, 110.0);
    a.on_event(mkt5);

    // Post-hoc: peak = 58000, trough = 51000, dd = (58000-51000)/58000
    double expected_dd = (58000.0 - 51000.0) / 58000.0 * 100.0;

    auto r = a.generate_report();
    EXPECT_NEAR(r.max_drawdown, expected_dd, 0.01);
}

TEST(Analytics, Snapshot_MidRun_ReturnsPartialMetrics)
{
    Analytics a;
    auto mkt = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt);

    // Do one winning trade
    auto bo = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 10, 100.0);
    bo->set_order_id(1);
    a.on_event(bo);
    auto bf = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 10, 100.0, 0.0);
    a.on_event(bf);

    auto mkt2 = std::make_shared<market_event>(epoch_ms(2), "X", 110, 110, 110, 110.0);
    a.on_event(mkt2);

    auto so = std::make_shared<order_event>(epoch_ms(3), "X", order_type::limit, order_side::sell, 10, 110.0);
    so->set_order_id(2);
    a.on_event(so);
    auto sf = std::make_shared<fill_event>(epoch_ms(3), "X", 2, order_side::sell, 10, 110.0, 0.0);
    a.on_event(sf);

    // Take snapshot mid-run (before generate_report)
    auto snap = a.snapshot();

    // Snapshot should have metrics but NO equity curve or trade log vectors
    EXPECT_EQ(snap.total_trades, 1u);
    EXPECT_EQ(snap.total_orders, 2u);
    EXPECT_EQ(snap.total_fills, 2u);
    EXPECT_GT(snap.cumulative_return, 0.0);
    EXPECT_DOUBLE_EQ(snap.win_rate, 100.0);
    EXPECT_TRUE(snap.equity_curve.empty());
    EXPECT_TRUE(snap.trade_returns.empty());
    EXPECT_TRUE(snap.trades.empty());

    // Full report should have the vectors populated
    auto full = a.generate_report();
    EXPECT_FALSE(full.equity_curve.empty());
    EXPECT_FALSE(full.trade_returns.empty());
    EXPECT_FALSE(full.trades.empty());
    EXPECT_EQ(full.total_trades, snap.total_trades);
    EXPECT_DOUBLE_EQ(full.sharpe_ratio, snap.sharpe_ratio);
}

// --- Signed-position model: shorts, pyramiding, flipping ---

TEST(Analytics, ShortRoundTrip_ProfitsWhenPriceFalls)
{
    Analytics a;
    auto mkt = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt);

    // Sell 10 @ 100 to open short
    auto so = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::sell, 10, 100.0);
    so->set_order_id(1);
    a.on_event(so);
    auto sf = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::sell, 10, 100.0, 0.0);
    a.on_event(sf);

    // Buy 10 @ 80 to close short -> profit 10 * (100 - 80) = 200
    auto bo = std::make_shared<order_event>(epoch_ms(2), "X", order_type::limit, order_side::buy, 10, 80.0);
    bo->set_order_id(2);
    a.on_event(bo);
    auto bf = std::make_shared<fill_event>(epoch_ms(2), "X", 2, order_side::buy, 10, 80.0, 0.0);
    a.on_event(bf);

    auto r = a.generate_report();
    EXPECT_EQ(r.total_trades, 1u);
    ASSERT_EQ(r.trade_returns.size(), 1u);
    EXPECT_NEAR(r.trade_returns[0], 200.0, 1e-9);
    EXPECT_DOUBLE_EQ(r.win_rate, 100.0);
}

TEST(Analytics, Pyramiding_WeightedAverageEntry)
{
    Analytics a;
    auto mkt = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt);

    // Buy 10 @ 100, then buy 10 @ 120 -> avg entry = 110 on 20 units
    auto bo1 = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 10, 100.0);
    bo1->set_order_id(1);
    a.on_event(bo1);
    auto bf1 = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 10, 100.0, 0.0);
    a.on_event(bf1);

    auto bo2 = std::make_shared<order_event>(epoch_ms(2), "X", order_type::limit, order_side::buy, 10, 120.0);
    bo2->set_order_id(2);
    a.on_event(bo2);
    auto bf2 = std::make_shared<fill_event>(epoch_ms(2), "X", 2, order_side::buy, 10, 120.0, 0.0);
    a.on_event(bf2);

    // Sell 20 @ 130 -> pnl = 20 * (130 - 110) = 400
    auto so = std::make_shared<order_event>(epoch_ms(3), "X", order_type::limit, order_side::sell, 20, 130.0);
    so->set_order_id(3);
    a.on_event(so);
    auto sf = std::make_shared<fill_event>(epoch_ms(3), "X", 3, order_side::sell, 20, 130.0, 0.0);
    a.on_event(sf);

    auto r = a.generate_report();
    EXPECT_EQ(r.total_trades, 1u);
    ASSERT_EQ(r.trade_returns.size(), 1u);
    EXPECT_NEAR(r.trade_returns[0], 400.0, 1e-9);
}

TEST(Analytics, Flipping_LongToShortInOneFill)
{
    Analytics a;
    auto mkt = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt);

    // Buy 10 @ 100 -> long 10
    auto bo = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 10, 100.0);
    bo->set_order_id(1);
    a.on_event(bo);
    auto bf = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 10, 100.0, 0.0);
    a.on_event(bf);

    // Sell 15 @ 120 -> closes 10 (pnl = 10 * 20 = 200), opens short 5 @ 120
    auto so = std::make_shared<order_event>(epoch_ms(2), "X", order_type::limit, order_side::sell, 15, 120.0);
    so->set_order_id(2);
    a.on_event(so);
    auto sf = std::make_shared<fill_event>(epoch_ms(2), "X", 2, order_side::sell, 15, 120.0, 0.0);
    a.on_event(sf);

    auto r = a.generate_report();
    // One closed round-trip so far
    EXPECT_EQ(r.total_trades, 1u);
    ASSERT_EQ(r.trade_returns.size(), 1u);
    EXPECT_NEAR(r.trade_returns[0], 200.0, 1e-9);

    // Buy 5 @ 100 -> closes short (pnl = 5 * (120 - 100) = 100)
    auto bo2 = std::make_shared<order_event>(epoch_ms(3), "X", order_type::limit, order_side::buy, 5, 100.0);
    bo2->set_order_id(3);
    a.on_event(bo2);
    auto bf2 = std::make_shared<fill_event>(epoch_ms(3), "X", 3, order_side::buy, 5, 100.0, 0.0);
    a.on_event(bf2);

    r = a.generate_report();
    EXPECT_EQ(r.total_trades, 2u);
    ASSERT_EQ(r.trade_returns.size(), 2u);
    EXPECT_NEAR(r.trade_returns[1], 100.0, 1e-9);
}

TEST(Analytics, FundingEvent_UpdatesCashAndEquityAndRiskView)
{
    Analytics a(100000.0);

    // Simulate a funding credit from the exchange
    auto funding_ev = std::make_shared<funding_event>(
        epoch_ms(1000), "BTCUSDT", 0.0, 250.0, "FUNDING_FEE");

    a.on_event(funding_ev);

    auto rv = a.risk_view();
    EXPECT_NEAR(rv.equity, 100250.0, 1e-6);
    EXPECT_NEAR(a.total_funding_pnl(), 250.0, 1e-6);

    auto report = a.generate_report();
    // Equity curve should have recorded the funding adjustment point
    EXPECT_GE(report.equity_curve.size(), 1u);
}

// risk_view().equity must be usable for max_position_pct_of_equity from t0,
// not only after a funding event (was stuck at 0 and fail-opened).
TEST(Analytics, RiskViewEquity_SeededAndUpdatedOnMarket)
{
    Analytics a(100000.0);
    EXPECT_NEAR(a.risk_view().equity, 100000.0, 1e-9);

    auto m = std::make_shared<market_event>(epoch_ms(1), "X",
                                            100.0, 100.0, 100.0, 100.0);
    a.on_event(m);
    EXPECT_NEAR(a.risk_view().equity, 100000.0, 1e-9);

    // Open long 10 @ 100, then mark to 110 → equity +100
    auto buy = std::make_shared<fill_event>(
        epoch_ms(2), "X", 1, order_side::buy, 10.0, 100.0, 0.0);
    a.on_event(buy);
    EXPECT_NEAR(a.risk_view().equity, 100000.0, 1e-6);

    auto m2 = std::make_shared<market_event>(epoch_ms(3), "X",
                                             110.0, 110.0, 110.0, 110.0);
    a.on_event(m2);
    EXPECT_NEAR(a.risk_view().equity, 100100.0, 1e-6);

    a.reset(50000.0);
    EXPECT_NEAR(a.risk_view().equity, 50000.0, 1e-9);
}
