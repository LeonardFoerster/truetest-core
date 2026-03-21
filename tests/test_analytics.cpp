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

TEST(Analytics, Welford_AccuracyVsBatch)
{
    // Feed known trade returns through Analytics, then compare Welford-derived
    // mean/stddev against a batch calculation.  Tolerance: 1e-10.
    Analytics a;
    auto mkt = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt);

    // Returns we will inject via buy/sell round-trips
    const std::vector<double> prices = {110.0, 95.0, 130.0, 85.0, 120.0, 105.0, 90.0, 115.0};
    const double entry_price = 100.0;
    const int qty = 10;

    // Compute expected returns (pnl = qty * (exit - entry))
    std::vector<double> expected_returns;
    for (double exit : prices)
        expected_returns.push_back(qty * (exit - entry_price));

    uint64_t oid = 1;
    for (std::size_t i = 0; i < prices.size(); ++i)
    {
        auto bo = std::make_shared<order_event>(epoch_ms(static_cast<int64_t>(i * 10 + 1)), "X",
            order_type::limit, order_side::buy, qty, entry_price);
        bo->set_order_id(oid);
        a.on_event(bo);
        auto bf = std::make_shared<fill_event>(epoch_ms(static_cast<int64_t>(i * 10 + 1)), "X",
            oid, order_side::buy, qty, entry_price, 0.0);
        a.on_event(bf);
        oid++;

        auto so = std::make_shared<order_event>(epoch_ms(static_cast<int64_t>(i * 10 + 2)), "X",
            order_type::limit, order_side::sell, qty, prices[i]);
        so->set_order_id(oid);
        a.on_event(so);
        auto sf = std::make_shared<fill_event>(epoch_ms(static_cast<int64_t>(i * 10 + 2)), "X",
            oid, order_side::sell, qty, prices[i], 0.0);
        a.on_event(sf);
        oid++;
    }

    // Batch calculation
    double sum = 0.0;
    for (double r : expected_returns) sum += r;
    double batch_mean = sum / static_cast<double>(expected_returns.size());

    double sum_sq = 0.0;
    for (double r : expected_returns)
        sum_sq += (r - batch_mean) * (r - batch_mean);
    double batch_stddev = std::sqrt(sum_sq / static_cast<double>(expected_returns.size() - 1));

    auto report = a.generate_report();
    double sharpe = (batch_stddev > 0.0) ? batch_mean / batch_stddev : 0.0;

    EXPECT_NEAR(report.sharpe_ratio, sharpe, 1e-10);
}

TEST(Analytics, RunningDrawdown_MatchesPostHoc)
{
    Analytics a(50000.0);

    // Simulate equity curve: 50000 → 55000 → 52000 → 58000 → 51000
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
