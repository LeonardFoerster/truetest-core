#include <array>
#include <cmath>
#include <iterator>
#include <limits>

#include <gtest/gtest.h>
#include "analytics/analytics.h"

static auto epoch_ms(int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

namespace {

// Keep a valid L2 dynamic type while exercising the signal switch branch, so
// the pre-fix fallthrough is deterministic without reproducing its bad cast.
class signal_tagged_l2_snapshot_event final : public l2_snapshot_event
{
public:
    using l2_snapshot_event::l2_snapshot_event;

    void retag_as_signal() noexcept { type_ = event_type::signal; }
};

} // namespace

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

TEST(Analytics, BF08_InvalidFillQuantityDoesNotMutateState)
{
    Analytics a(1000.0);
    auto order = std::make_shared<order_event>(
        epoch_ms(0), "X", order_type::limit, order_side::buy, 2.0, 100.0);
    order->set_order_id(1);
    order->set_strategy_name("bf08");
    a.on_event(order);

    auto open = std::make_shared<fill_event>(
        epoch_ms(1), "X", 1, order_side::buy, 2.0, 100.0, 1.0);
    a.on_event(open);

    const auto before = a.generate_report();
    ASSERT_EQ(before.open_positions.size(), 1u);

    const std::array invalid_quantities = {
        0.0,
        -1.0,
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
    };
    int64_t timestamp_ms = 2;
    for (const double quantity : invalid_quantities)
    {
        auto invalid = std::make_shared<fill_event>(
            epoch_ms(timestamp_ms), "X", 1,
            order_side::sell, quantity, 110.0, 7.0);
        invalid->set_latency_ns(123);
        a.on_event(invalid);
        ++timestamp_ms;
    }

    const auto after = a.generate_report();
    ASSERT_EQ(after.open_positions.size(), 1u);
    EXPECT_EQ(after.total_fills, before.total_fills);
    EXPECT_EQ(after.total_trades, before.total_trades);
    EXPECT_EQ(after.trades.size(), before.trades.size());
    EXPECT_DOUBLE_EQ(after.avg_slippage, before.avg_slippage);
    EXPECT_EQ(after.tick_to_trade_samples, before.tick_to_trade_samples);
    EXPECT_DOUBLE_EQ(after.avg_tick_to_trade_ns, before.avg_tick_to_trade_ns);
    EXPECT_EQ(after.per_symbol.size(), before.per_symbol.size());
    EXPECT_EQ(after.per_strategy.size(), before.per_strategy.size());
    EXPECT_DOUBLE_EQ(after.final_equity, before.final_equity);
    EXPECT_DOUBLE_EQ(after.realized_pnl, before.realized_pnl);
    EXPECT_DOUBLE_EQ(after.unrealized_pnl, before.unrealized_pnl);
    EXPECT_EQ(after.open_positions[0].symbol, before.open_positions[0].symbol);
    EXPECT_DOUBLE_EQ(after.open_positions[0].quantity, before.open_positions[0].quantity);
    EXPECT_DOUBLE_EQ(after.open_positions[0].avg_entry, before.open_positions[0].avg_entry);
    EXPECT_DOUBLE_EQ(after.open_positions[0].mark, before.open_positions[0].mark);
    EXPECT_DOUBLE_EQ(after.open_positions[0].unrealized_pnl,
                     before.open_positions[0].unrealized_pnl);
    EXPECT_TRUE(std::isfinite(after.final_equity));
    EXPECT_TRUE(std::isfinite(after.realized_pnl));
    EXPECT_TRUE(std::isfinite(after.unrealized_pnl));
}

TEST(Analytics, OpenPosition_ReportsUnrealizedPnl)
{
    Analytics a(10000.0);
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
    ASSERT_EQ(r.open_positions.size(), 1u);
    EXPECT_EQ(r.open_positions[0].symbol, "X");
    EXPECT_DOUBLE_EQ(r.open_positions[0].quantity, 10.0);
    EXPECT_DOUBLE_EQ(r.open_positions[0].avg_entry, 100.0);
    EXPECT_DOUBLE_EQ(r.open_positions[0].mark, 110.0);
    EXPECT_DOUBLE_EQ(r.open_positions[0].unrealized_pnl, 100.0); // (110-100)*10
    EXPECT_DOUBLE_EQ(r.unrealized_pnl, 100.0);
    EXPECT_DOUBLE_EQ(r.realized_pnl, 0.0);
}

TEST(Analytics, OpenPosition_UnrealizedIncludesRemainingEntryCommission)
{
    Analytics a(10000.0);

    auto buy = std::make_shared<fill_event>(
        epoch_ms(1), "X", 1, order_side::buy, 10.0, 100.0, 10.0);
    a.on_event(buy);

    auto partial_close = std::make_shared<fill_event>(
        epoch_ms(2), "X", 2, order_side::sell, 4.0, 100.0, 4.0);
    a.on_event(partial_close);

    const auto r = a.generate_report();
    ASSERT_EQ(r.open_positions.size(), 1u);
    EXPECT_DOUBLE_EQ(r.open_positions[0].quantity, 6.0);
    EXPECT_DOUBLE_EQ(r.open_positions[0].unrealized_pnl, -6.0);
    EXPECT_DOUBLE_EQ(r.realized_pnl, -8.0);
    EXPECT_DOUBLE_EQ(r.unrealized_pnl, -6.0);
    EXPECT_DOUBLE_EQ(r.final_equity, 9986.0);
    EXPECT_DOUBLE_EQ(r.final_equity - r.initial_equity,
                     r.realized_pnl + r.unrealized_pnl);
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

TEST(Analytics, BF06_RealSignalEventDoesNotReachL2Handler)
{
    Analytics a;
    auto signal = std::make_shared<signal_event>(
        epoch_ms(0), "X", signal_type::buy, 0.85);

    a.on_event(signal);

    EXPECT_DOUBLE_EQ(a.risk_view().current_spread_bps, 0.0);
}

TEST(Analytics, BF06_SignalTagDoesNotFallThroughToL2Handler)
{
    const l2_level bids[] = {{100.0, 10}};
    const l2_level asks[] = {{102.0, 10}};

    Analytics signal_dispatch;
    auto signal_tagged_snapshot = std::make_shared<signal_tagged_l2_snapshot_event>(
        epoch_ms(0), "X", bids, std::size(bids), asks, std::size(asks));
    signal_tagged_snapshot->retag_as_signal();
    signal_dispatch.on_event(signal_tagged_snapshot);
    EXPECT_DOUBLE_EQ(signal_dispatch.risk_view().current_spread_bps, 0.0);

    Analytics l2_dispatch;
    auto snapshot = std::make_shared<l2_snapshot_event>(
        epoch_ms(0), "X", bids, std::size(bids), asks, std::size(asks));
    l2_dispatch.on_event(snapshot);
    EXPECT_GT(l2_dispatch.risk_view().current_spread_bps, 0.0);
}

TEST(Analytics, L2SnapshotUpdatesSpreadMarkedEquityAndDrawdownTogether)
{
    Analytics a(1000.0);
    auto fill = std::make_shared<fill_event>(
        epoch_ms(0), "X", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1);
    a.on_event(fill);

    const l2_level first_bids[] = {{99.0, 10}};
    const l2_level first_asks[] = {{101.0, 10}};
    a.on_event(std::make_shared<l2_snapshot_event>(
        epoch_ms(1), "X", first_bids, std::size(first_bids),
        first_asks, std::size(first_asks)));
    EXPECT_DOUBLE_EQ(a.risk_view().equity, 1000.0);

    const l2_level down_bids[] = {{79.0, 10}};
    const l2_level down_asks[] = {{81.0, 10}};
    a.on_event(std::make_shared<l2_snapshot_event>(
        epoch_ms(2), "X", down_bids, std::size(down_bids),
        down_asks, std::size(down_asks)));

    const auto risk = a.risk_view();
    EXPECT_DOUBLE_EQ(risk.equity, 980.0);
    EXPECT_NEAR(risk.max_drawdown, 2.0, 1e-12);
    EXPECT_NEAR(risk.current_spread_bps, 250.0, 1e-12);
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

// --- Per-symbol position state (multi-symbol correctness) ---

TEST(Analytics, MultiSymbol_PositionsDoNotNetAcrossSymbols)
{
    Analytics a;
    auto mkt = std::make_shared<market_event>(epoch_ms(0), "A", 100, 100, 100, 100.0);
    a.on_event(mkt);

    // Long 10 A @ 100
    auto bf_a = std::make_shared<fill_event>(epoch_ms(1), "A", 1, order_side::buy, 10, 100.0, 0.0);
    a.on_event(bf_a);
    // Short 10 B @ 50 — must OPEN a short on B, not close the A long.
    auto sf_b = std::make_shared<fill_event>(epoch_ms(2), "B", 2, order_side::sell, 10, 50.0, 0.0);
    a.on_event(sf_b);

    auto r0 = a.generate_report();
    EXPECT_EQ(r0.total_trades, 0u) << "cross-symbol fill must not be booked as a close";

    // Close A @ 110 → +100; close B @ 40 → +100.
    auto sf_a = std::make_shared<fill_event>(epoch_ms(3), "A", 3, order_side::sell, 10, 110.0, 0.0);
    a.on_event(sf_a);
    auto bf_b = std::make_shared<fill_event>(epoch_ms(4), "B", 4, order_side::buy, 10, 40.0, 0.0);
    a.on_event(bf_b);

    auto r = a.generate_report();
    EXPECT_EQ(r.total_trades, 2u);
    ASSERT_EQ(r.trade_returns.size(), 2u);
    EXPECT_NEAR(r.trade_returns[0], 100.0, 1e-9);
    EXPECT_NEAR(r.trade_returns[1], 100.0, 1e-9);
    EXPECT_NEAR(r.per_symbol.at("A").total_pnl, 100.0, 1e-9);
    EXPECT_NEAR(r.per_symbol.at("B").total_pnl, 100.0, 1e-9);
}

TEST(Analytics, MultiSymbol_EquityMarksEachSymbolAtItsOwnPrice)
{
    Analytics a;
    auto m_a = std::make_shared<market_event>(epoch_ms(0), "A", 100, 100, 100, 100.0);
    a.on_event(m_a);
    auto m_b = std::make_shared<market_event>(epoch_ms(1), "B", 10, 10, 10, 10.0);
    a.on_event(m_b);

    // Long 10 A @ 100 and 10 B @ 10 → cash = 100000 - 1000 - 100 = 98900
    auto bf_a = std::make_shared<fill_event>(epoch_ms(2), "A", 1, order_side::buy, 10, 100.0, 0.0);
    a.on_event(bf_a);
    auto bf_b = std::make_shared<fill_event>(epoch_ms(3), "B", 2, order_side::buy, 10, 10.0, 0.0);
    a.on_event(bf_b);

    // A moves to 110, B stays 10: equity = 98900 + 1100 + 100 = 100100.
    auto m_a2 = std::make_shared<market_event>(epoch_ms(4), "A", 110, 110, 110, 110.0);
    a.on_event(m_a2);

    auto r = a.generate_report();
    EXPECT_NEAR(r.equity_curve.back().equity, 100100.0, 1e-6);
    EXPECT_NEAR(r.final_equity, 100100.0, 1e-6);
}

TEST(Analytics, RiskView_EquityPopulatedFromMarketEvents)
{
    Analytics a(100000.0);
    EXPECT_DOUBLE_EQ(a.risk_view().equity, 0.0); // nothing seen yet

    auto m = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(m);
    EXPECT_NEAR(a.risk_view().equity, 100000.0, 1e-6);

    auto bf = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 10, 100.0, 0.0);
    a.on_event(bf);
    auto m2 = std::make_shared<market_event>(epoch_ms(2), "X", 110, 110, 110, 110.0);
    a.on_event(m2);
    EXPECT_NEAR(a.risk_view().equity, 100100.0, 1e-6);
}

TEST(Analytics, RiskViewUpdatesEquityAndDrawdownImmediatelyAfterFill)
{
    Analytics a(1000.0);
    a.on_event(std::make_shared<market_event>(
        epoch_ms(0), "X", 100.0, 100.0, 100.0, 100.0));
    a.on_event(std::make_shared<fill_event>(
        epoch_ms(1), "X", 1, order_side::buy, 10.0, 100.0, 0.0));

    // No later market event: the close itself must make the loss visible to
    // the post-fill RiskManager in the same engine event.
    a.on_event(std::make_shared<fill_event>(
        epoch_ms(2), "X", 2, order_side::sell, 10.0, 80.0, 0.0));

    const auto rv = a.risk_view();
    EXPECT_DOUBLE_EQ(rv.equity, 800.0);
    EXPECT_DOUBLE_EQ(rv.max_drawdown, 20.0);
}

TEST(Analytics, TickAndFundingUpdateDrawdownImmediately)
{
    Analytics a(1000.0);
    a.on_event(std::make_shared<market_event>(
        epoch_ms(0), "X", 100.0, 100.0, 100.0, 100.0));
    a.on_event(std::make_shared<fill_event>(
        epoch_ms(1), "X", 1, order_side::buy, 10.0, 100.0, 0.0));
    a.on_event(std::make_shared<tick_event>(
        epoch_ms(2), "X", 150.0, 1, tick_side::bid));
    a.on_event(std::make_shared<tick_event>(
        epoch_ms(3), "X", 80.0, 1, tick_side::bid));

    auto rv = a.risk_view();
    EXPECT_NEAR(rv.equity, 800.0, 1e-9);
    EXPECT_NEAR(rv.max_drawdown, 700.0 / 1500.0 * 100.0, 1e-9);

    a.on_event(std::make_shared<funding_event>(
        epoch_ms(4), "X", 0.0, -100.0, "FUNDING_FEE"));
    rv = a.risk_view();
    EXPECT_NEAR(rv.equity, 700.0, 1e-9);
    EXPECT_NEAR(rv.max_drawdown, 800.0 / 1500.0 * 100.0, 1e-9);
}

TEST(Analytics, Sortino_DownsideDeviationOverAllPeriods)
{
    const std::size_t ppy = 252;
    Analytics a(100000.0, 252, 0.0, ppy);

    auto mkt0 = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt0);
    auto bf = std::make_shared<fill_event>(epoch_ms(0), "X", 1, order_side::buy, 100, 100.0, 0.0);
    a.on_event(bf);
    // cash = 90000, pos = 100 → equity = 90000 + 100 * p

    const std::vector<double> prices = {102.0, 101.0, 105.0, 103.0};
    std::vector<double> rets;
    double prev_eq = 100000.0;
    for (double p : prices)
    {
        double eq = 90000.0 + 100.0 * p;
        rets.push_back((eq - prev_eq) / prev_eq);
        prev_eq = eq;
    }

    int64_t t = 1;
    for (double p : prices)
    {
        auto m = std::make_shared<market_event>(epoch_ms(t++), "X", p, p, p, p);
        a.on_event(m);
    }

    double mean = 0.0;
    for (double r : rets) mean += r;
    mean /= static_cast<double>(rets.size());

    // Downside deviation: sqrt(mean over ALL periods of min(r, 0)^2), MAR = 0.
    double dsq = 0.0;
    for (double r : rets) if (r < 0.0) dsq += r * r;
    double downside_dev = std::sqrt(dsq / static_cast<double>(rets.size()));
    double expected_sortino = (mean / downside_dev) * std::sqrt(static_cast<double>(ppy));

    auto report = a.generate_report();
    EXPECT_NEAR(report.sortino_ratio, expected_sortino, 1e-9);
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

TEST(Analytics, FillCarriesStrategyWhenRecordedOrderIsUnavailable)
{
    Analytics a(10'000.0);
    auto fill = std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 77, order_side::buy, 1.0, 100.0,
        0.0, 0.0, 1, "ledger_strategy", 77);

    a.on_event(fill);
    const auto report = a.generate_report();
    ASSERT_EQ(report.trades.size(), 1u);
    EXPECT_EQ(report.trades.front().strategy_name, "ledger_strategy");
}

TEST(Analytics, MultiStrategyConcurrentPositions_IsolatedPnL)
{
    Analytics a(100000.0);

    // Strategy A opens 10 BTC Long @ 50,000 (Order 101)
    auto fill_a_open = std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 101, order_side::buy, 10.0, 50000.0,
        0.0, 0.0, 1, "strat_a", 101);
    a.on_event(fill_a_open);

    // Strategy B opens 3 BTC Short @ 51,000 (Order 201)
    auto fill_b_open = std::make_shared<fill_event>(
        epoch_ms(2), "BTCUSDT", 201, order_side::sell, 3.0, 51000.0,
        0.0, 0.0, 1, "strat_b", 201);
    a.on_event(fill_b_open);

    // At this point, no trades have closed yet; both positions are open and isolated.
    auto r_mid = a.generate_report();
    EXPECT_EQ(r_mid.total_trades, 0u);
    EXPECT_DOUBLE_EQ(r_mid.realized_pnl, 0.0);

    // Strategy A closes 10 BTC Long @ 55,000 (Order 102) -> PnL: +50,000
    auto fill_a_close = std::make_shared<fill_event>(
        epoch_ms(3), "BTCUSDT", 102, order_side::sell, 10.0, 55000.0,
        0.0, 0.0, 1, "strat_a", 101);
    a.on_event(fill_a_close);

    // Strategy B closes 3 BTC Short @ 49,000 (Order 202) -> PnL: (51000 - 49000) * 3 = +6,000
    auto fill_b_close = std::make_shared<fill_event>(
        epoch_ms(4), "BTCUSDT", 202, order_side::buy, 3.0, 49000.0,
        0.0, 0.0, 1, "strat_b", 201);
    a.on_event(fill_b_close);

    auto r_final = a.generate_report();
    EXPECT_EQ(r_final.total_trades, 2u);
    EXPECT_DOUBLE_EQ(r_final.realized_pnl, 56000.0);

    ASSERT_EQ(r_final.per_strategy.count("strat_a"), 1u);
    EXPECT_DOUBLE_EQ(r_final.per_strategy.at("strat_a").total_pnl, 50000.0);
    EXPECT_EQ(r_final.per_strategy.at("strat_a").trade_count, 1u);

    ASSERT_EQ(r_final.per_strategy.count("strat_b"), 1u);
    EXPECT_DOUBLE_EQ(r_final.per_strategy.at("strat_b").total_pnl, 6000.0);
    EXPECT_EQ(r_final.per_strategy.at("strat_b").trade_count, 1u);
}
