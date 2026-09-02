#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "analytics/analytics.h"

static auto epoch_ms(int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

namespace {

// Analytics is downstream of the canonical fill ingress. Most historical
// tests constructed observer events directly and therefore omitted the
// stable identity that a supported engine path guarantees. Give ordinary
// fixtures deterministic process-unique identities; negative identity tests
// deliberately call Analytics::on_event directly.
void deliver_valid_event(Analytics& analytics, const event_pointer& event)
{
    static std::uint64_t next_fill_id = 1;
    if (event && event->get_type() == event_type::fill)
    {
        auto fill = std::static_pointer_cast<fill_event>(event);
        if (fill->get_fill_id() == 0
            && fill->get_venue_execution_id().empty())
            fill->set_fill_id(next_fill_id++);
        if (fill->get_commission() != 0.0
            && fill->get_commission_currency().empty())
        {
            EXPECT_TRUE(fill->set_commission_currency("USD"));
        }
    }
    analytics.on_event(event);
}

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
    EXPECT_FALSE(r.profit_factor_valid);
    EXPECT_FALSE(r.profit_factor_unbounded);
    EXPECT_EQ(r.profit_factor_reason, "no_winning_or_losing_trades");
}

TEST(Analytics, ProfitFactorWithoutLossesIsExplicitlyUnbounded)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "X", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(2), "X", 2, order_side::sell,
        1.0, 110.0, 0.0, 0.0, 2));

    const auto report = analytics.generate_report();
    EXPECT_DOUBLE_EQ(report.profit_factor, 0.0);
    EXPECT_DOUBLE_EQ(report.total_win, 10.0);
    EXPECT_DOUBLE_EQ(report.total_loss, 0.0);
    EXPECT_FALSE(report.profit_factor_valid);
    EXPECT_TRUE(report.profit_factor_unbounded);
    EXPECT_EQ(report.profit_factor_reason, "no_losses_unbounded");
    ASSERT_EQ(report.per_symbol.size(), 1u);
    const auto& symbol = report.per_symbol.begin()->second;
    EXPECT_DOUBLE_EQ(symbol.profit_factor(), 0.0);
    EXPECT_FALSE(symbol.profit_factor_valid());
    EXPECT_TRUE(symbol.profit_factor_unbounded());
}

TEST(Analytics, SortinoWithoutDownsideIsExplicitlyUnbounded)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "X", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    for (int minute = 1; minute <= 4; ++minute)
    {
        const double price = 100.0 + static_cast<double>(minute);
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(minute * 60'000), "X",
            price, price, price, price));
    }

    const auto report = analytics.generate_report();
    EXPECT_DOUBLE_EQ(report.sortino_ratio, 0.0);
    EXPECT_FALSE(report.sortino_ratio_valid);
    EXPECT_EQ(report.sortino_ratio_reason,
              "no_downside_observed_unbounded");
}

TEST(Analytics, DefaultsToCryptoMinuteAnnualization)
{
    Analytics a;
    EXPECT_EQ(a.snapshot().periods_per_year, 525600u);
}

TEST(Analytics, InitialCashMustBeFiniteAndPositive)
{
    EXPECT_THROW((void)Analytics(0.0), std::invalid_argument);
    EXPECT_THROW((void)Analytics(-1.0), std::invalid_argument);
    EXPECT_THROW((void)Analytics(
                     std::numeric_limits<double>::quiet_NaN()),
                 std::invalid_argument);
    EXPECT_THROW((void)Analytics(
                     std::numeric_limits<double>::infinity()),
                 std::invalid_argument);
    EXPECT_THROW((void)Analytics(
                     1'000.0, 30,
                     std::numeric_limits<double>::quiet_NaN()),
                 std::invalid_argument);

    Analytics analytics(1'000.0);
    const auto before = analytics.generate_report();
    EXPECT_THROW(analytics.reset(0.0), std::invalid_argument);
    const auto after = analytics.generate_report();
    EXPECT_DOUBLE_EQ(after.initial_equity, before.initial_equity);
    EXPECT_DOUBLE_EQ(after.final_equity, before.final_equity);
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
        deliver_valid_event(a, m);
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
    deliver_valid_event(a, m1);
    auto buy = std::make_shared<order_event>(epoch_ms(1), "X",
                  order_type::limit, order_side::buy, 100.0, 100.0);
    buy->set_order_id(1);
    deliver_valid_event(a, buy);
    auto fill = std::make_shared<fill_event>(epoch_ms(1), "X", 1,
                  order_side::buy, 100.0, 100.0, 0.0);
    deliver_valid_event(a, fill);

    // Price rises to 110 -> equity peak at ~101000.
    auto m2 = std::make_shared<market_event>(epoch_ms(2), "X", 110, 110, 110, 110.0);
    deliver_valid_event(a, m2);
    // Then drops to 105 -> drawdown vs peak.
    auto m3 = std::make_shared<market_event>(epoch_ms(3), "X", 105, 105, 105, 105.0);
    deliver_valid_event(a, m3);

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
    deliver_valid_event(a, m1);
    deliver_valid_event(a, m2);
    deliver_valid_event(a, m3);

    auto r = a.generate_report();
    EXPECT_EQ(r.equity_curve.size(), 3u);
    // Buy-and-hold: (120 - 100) / 100 = 0.2
    EXPECT_NEAR(r.buy_and_hold_return, 0.2, 0.001);
}

TEST(Analytics, ExtremeFiniteBenchmarkPathFailsClosedAndExportsStrictJson)
{
    Analytics analytics(1'000.0);
    const double tiny = std::numeric_limits<double>::min();
    const double huge = std::numeric_limits<double>::max();
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(1), "X", tiny, tiny, tiny, tiny));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(2), "X", huge, huge, huge, huge));

    const auto report = analytics.generate_report();
    EXPECT_FALSE(report.benchmark_valid);
    EXPECT_EQ(report.benchmark_reason, "non_finite_benchmark_path");
    EXPECT_TRUE(std::isfinite(report.final_equity));
    EXPECT_TRUE(std::isfinite(analytics.risk_view().realized_vol_1h));

    const auto path = std::filesystem::temp_directory_path()
        / "truetest_extreme_benchmark.json";
    analytics.export_json(path.string());
    std::ifstream in(path);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), {});
    std::filesystem::remove(path);
    const auto parsed = nlohmann::json::parse(json);
    EXPECT_FALSE(parsed.at("benchmark_valid").get<bool>());
}

TEST(Analytics, NonRepresentablePortfolioReturnFailsClosed)
{
    const double tiny = std::numeric_limits<double>::min();
    const double huge = std::numeric_limits<double>::max();
    Analytics analytics(tiny);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "X", 1, order_side::buy,
        1.0, tiny, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(2), "X", tiny, tiny, tiny, tiny));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(3), "X", huge, huge, huge, huge));

    const auto report = analytics.generate_report();
    EXPECT_FALSE(report.portfolio_time_series_valid);
    EXPECT_EQ(report.portfolio_time_series_reason,
              "non_finite_portfolio_return");
    EXPECT_FALSE(report.sharpe_ratio_valid);
    EXPECT_FALSE(report.sortino_ratio_valid);
    EXPECT_FALSE(report.annualized_return_valid);
    EXPECT_TRUE(std::isfinite(report.cumulative_return));
    EXPECT_TRUE(std::isfinite(report.max_drawdown));

    const auto path = std::filesystem::temp_directory_path()
        / "truetest_nonrepresentable_portfolio_return.json";
    analytics.export_json(path.string());
    std::ifstream in(path);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), {});
    std::filesystem::remove(path);
    const auto parsed = nlohmann::json::parse(json);
    EXPECT_FALSE(parsed.at("portfolio_time_series_valid").get<bool>());
}

TEST(Analytics, OrderEvent_RecordsIntendedPrice)
{
    Analytics a;
    auto mkt = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    deliver_valid_event(a, mkt);

    auto ord = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 10, 100.0);
    ord->set_order_id(1);
    deliver_valid_event(a, ord);

    auto r = a.generate_report();
    EXPECT_EQ(r.total_orders, 1u);
}

TEST(Analytics, FillProvenanceIsExportedAsExploratoryExecutionEvidence)
{
    Analytics a;
    auto fill = std::make_shared<fill_event>(
        epoch_ms(5), "X", 1, order_side::buy, 2.0, 101.0, 0.25,
        /*remaining=*/0.0, /*fill_id=*/77);
    fill_provenance provenance;
    provenance.model = fill_execution_model::synthetic_local_liquidity;
    provenance.reason = fill_execution_reason::aggressive_ladder_match;
    provenance.exploratory = true;
    provenance.intended_price = 100.0;
    provenance.reference_price = 100.0;
    provenance.reference_timestamp = epoch_ms(5);
    provenance.modeled_spread_bps = 100.0;
    provenance.modeled_impact_bps = 5.0;
    provenance.fill_probability = 1.0;
    provenance.modeled_latency = std::chrono::milliseconds(5);
    fill->set_provenance(provenance);
    deliver_valid_event(a, fill);

    const auto report = a.generate_report();
    ASSERT_EQ(report.trades.size(), 1u);
    const auto& trade = report.trades.front();
    EXPECT_EQ(trade.execution_model, fill_execution_model::synthetic_local_liquidity);
    EXPECT_EQ(trade.execution_reason, fill_execution_reason::aggressive_ladder_match);
    EXPECT_TRUE(trade.execution_is_exploratory);
    EXPECT_DOUBLE_EQ(trade.intended_price, 100.0);
    EXPECT_DOUBLE_EQ(trade.reference_price, 100.0);
    EXPECT_EQ(trade.reference_timestamp, epoch_ms(5));
    EXPECT_DOUBLE_EQ(trade.modeled_spread_bps, 100.0);
    EXPECT_DOUBLE_EQ(trade.modeled_impact_bps, 5.0);
    EXPECT_DOUBLE_EQ(trade.fill_probability, 1.0);
    EXPECT_EQ(trade.modeled_latency, std::chrono::milliseconds(5));
    EXPECT_EQ(trade.fill_id, 77u);
    EXPECT_TRUE(report.contains_exploratory_execution);
}

TEST(Analytics, NonFiniteOrNonCausalFillProvenanceFailsClosed)
{
    Analytics analytics(1'000.0);
    std::uint64_t next_id = 1;
    const auto reject = [&](fill_provenance provenance) {
        auto fill = std::make_shared<fill_event>(
            epoch_ms(10), "X", next_id, order_side::buy,
            1.0, 100.0, 0.0, 0.0, next_id);
        ++next_id;
        fill->set_provenance(provenance);
        deliver_valid_event(analytics, fill);
    };

    fill_provenance provenance;
    provenance.intended_price = std::numeric_limits<double>::infinity();
    reject(provenance);
    provenance = {};
    provenance.reference_price = std::numeric_limits<double>::quiet_NaN();
    reject(provenance);
    provenance = {};
    provenance.modeled_spread_bps = std::numeric_limits<double>::infinity();
    reject(provenance);
    provenance = {};
    provenance.modeled_impact_bps = std::numeric_limits<double>::quiet_NaN();
    reject(provenance);
    provenance = {};
    provenance.fill_probability = -0.01;
    reject(provenance);
    provenance.fill_probability = 1.01;
    reject(provenance);
    provenance.fill_probability = std::numeric_limits<double>::infinity();
    reject(provenance);
    provenance = {};
    provenance.modeled_latency = std::chrono::nanoseconds{-1};
    reject(provenance);
    provenance = {};
    provenance.reference_timestamp = epoch_ms(11);
    reject(provenance);

    const auto report = analytics.generate_report();
    EXPECT_EQ(report.total_fills, 0u);
    EXPECT_TRUE(report.trades.empty());
    EXPECT_TRUE(report.open_positions.empty());
    EXPECT_EQ(report.invalid_fill_payloads_rejected, 9u);
    EXPECT_FALSE(report.accounting_reconciled);
    EXPECT_EQ(report.accounting_reconciliation_reason,
              "unreconciled_fill_event");
    EXPECT_FALSE(report.valuation_complete);

    const auto path = std::filesystem::temp_directory_path()
        / "truetest_invalid_fill_provenance.json";
    analytics.export_json(path.string());
    std::ifstream in(path);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), {});
    std::filesystem::remove(path);
    const auto parsed = nlohmann::json::parse(json);
    EXPECT_EQ(parsed.at("invalid_fill_payloads_rejected"), 9u);
}

TEST(Analytics, FillEvent_BuyUpdatesEquity)
{
    Analytics a;
    auto mkt = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    deliver_valid_event(a, mkt);

    auto ord = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 10, 100.0);
    ord->set_order_id(1);
    deliver_valid_event(a, ord);

    auto fill = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 10, 100.0, 0.0);
    deliver_valid_event(a, fill);

    auto mkt2 = std::make_shared<market_event>(epoch_ms(2), "X", 110, 110, 110, 110.0);
    deliver_valid_event(a, mkt2);

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
    deliver_valid_event(a, order);

    auto open = std::make_shared<fill_event>(
        epoch_ms(1), "X", 1, order_side::buy, 2.0, 100.0, 1.0);
    deliver_valid_event(a, open);

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
        deliver_valid_event(a, invalid);
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
    deliver_valid_event(a, mkt);

    auto ord = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 10, 100.0);
    ord->set_order_id(1);
    deliver_valid_event(a, ord);
    auto fill = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 10, 100.0, 0.0);
    deliver_valid_event(a, fill);

    auto mkt2 = std::make_shared<market_event>(epoch_ms(2), "X", 110, 110, 110, 110.0);
    deliver_valid_event(a, mkt2);

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

TEST(Analytics, FillOnlyValuationIsExplicitlyProvisional)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));

    const auto provisional = analytics.generate_report();
    ASSERT_EQ(provisional.open_positions.size(), 1u);
    EXPECT_FALSE(provisional.valuation_complete);
    EXPECT_EQ(provisional.valuation_reason,
              "open_position_without_market_mark");
    EXPECT_FALSE(provisional.open_positions.front().mark_valid);
    EXPECT_EQ(provisional.open_positions.front().mark_source,
              "fill_price_provisional");

    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(2), "A", 101.0, 101.0, 101.0, 101.0));
    const auto marked = analytics.generate_report();
    ASSERT_EQ(marked.open_positions.size(), 1u);
    EXPECT_TRUE(marked.valuation_complete);
    EXPECT_TRUE(marked.open_positions.front().mark_valid);
    EXPECT_EQ(marked.open_positions.front().mark_source, "market_mark");
}

TEST(Analytics, OpenPosition_UnrealizedIncludesRemainingEntryCommission)
{
    Analytics a(10000.0);

    auto buy = std::make_shared<fill_event>(
        epoch_ms(1), "X", 1, order_side::buy, 10.0, 100.0, 10.0);
    deliver_valid_event(a, buy);

    auto partial_close = std::make_shared<fill_event>(
        epoch_ms(2), "X", 2, order_side::sell, 4.0, 100.0, 4.0);
    deliver_valid_event(a, partial_close);

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
    deliver_valid_event(a, mkt1);

    auto buy_ord = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 10, 100.0);
    buy_ord->set_order_id(1);
    deliver_valid_event(a, buy_ord);

    auto buy_fill = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 10, 100.0, 0.0);
    deliver_valid_event(a, buy_fill);

    auto mkt2 = std::make_shared<market_event>(epoch_ms(2), "X", 110, 110, 110, 110.0);
    deliver_valid_event(a, mkt2);

    auto sell_ord = std::make_shared<order_event>(epoch_ms(3), "X", order_type::limit, order_side::sell, 10, 110.0);
    sell_ord->set_order_id(2);
    deliver_valid_event(a, sell_ord);

    auto sell_fill = std::make_shared<fill_event>(epoch_ms(3), "X", 2, order_side::sell, 10, 110.0, 0.0);
    deliver_valid_event(a, sell_fill);

    auto r = a.generate_report();
    EXPECT_EQ(r.total_trades, 1u);
    EXPECT_GT(r.trade_returns[0], 0.0); // profitable trade
}

TEST(Analytics, WinRate)
{
    Analytics a;
    auto mkt = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    deliver_valid_event(a, mkt);

    uint64_t oid = 1;
    auto round_trip = [&](double entry, double exit, int64_t t_start) {
        auto bo = std::make_shared<order_event>(epoch_ms(t_start), "X", order_type::limit, order_side::buy, 10, entry);
        bo->set_order_id(oid);
        deliver_valid_event(a, bo);
        auto bf = std::make_shared<fill_event>(epoch_ms(t_start), "X", oid, order_side::buy, 10, entry, 0.0);
        deliver_valid_event(a, bf);
        oid++;
        auto so = std::make_shared<order_event>(epoch_ms(t_start + 1), "X", order_type::limit, order_side::sell, 10, exit);
        so->set_order_id(oid);
        deliver_valid_event(a, so);
        auto sf = std::make_shared<fill_event>(epoch_ms(t_start + 1), "X", oid, order_side::sell, 10, exit, 0.0);
        deliver_valid_event(a, sf);
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
    deliver_valid_event(a, tick);
    deliver_valid_event(a, l2);
    auto r = a.generate_report();
    EXPECT_EQ(r.total_fills, 0u);
}

TEST(Analytics, BF06_RealSignalEventDoesNotReachL2Handler)
{
    Analytics a;
    auto signal = std::make_shared<signal_event>(
        epoch_ms(0), "X", signal_type::buy, 0.85);

    deliver_valid_event(a, signal);

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
    deliver_valid_event(signal_dispatch, signal_tagged_snapshot);
    EXPECT_DOUBLE_EQ(signal_dispatch.risk_view().current_spread_bps, 0.0);

    Analytics l2_dispatch;
    auto snapshot = std::make_shared<l2_snapshot_event>(
        epoch_ms(0), "X", bids, std::size(bids), asks, std::size(asks));
    deliver_valid_event(l2_dispatch, snapshot);
    EXPECT_GT(l2_dispatch.risk_view().current_spread_bps, 0.0);
}

TEST(Analytics, L2SnapshotUpdatesSpreadMarkedEquityAndDrawdownTogether)
{
    Analytics a(1000.0);
    auto fill = std::make_shared<fill_event>(
        epoch_ms(0), "X", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1);
    deliver_valid_event(a, fill);

    const l2_level first_bids[] = {{99.0, 10}};
    const l2_level first_asks[] = {{101.0, 10}};
    deliver_valid_event(a, std::make_shared<l2_snapshot_event>(
        epoch_ms(1), "X", first_bids, std::size(first_bids),
        first_asks, std::size(first_asks)));
    EXPECT_DOUBLE_EQ(a.risk_view().equity, 1000.0);

    const l2_level down_bids[] = {{79.0, 10}};
    const l2_level down_asks[] = {{81.0, 10}};
    deliver_valid_event(a, std::make_shared<l2_snapshot_event>(
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
    deliver_valid_event(a, mkt0);
    auto bo = std::make_shared<order_event>(epoch_ms(0), "X", order_type::limit, order_side::buy, 100, 100.0);
    bo->set_order_id(1);
    deliver_valid_event(a, bo);
    auto bf = std::make_shared<fill_event>(epoch_ms(0), "X", 1, order_side::buy, 100, 100.0, 0.0);
    deliver_valid_event(a, bf);
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

    const auto period = std::chrono::duration_cast<
        std::chrono::system_clock::duration>(std::chrono::days{365})
        / static_cast<std::int64_t>(ppy);
    auto t = epoch_ms(0) + period;
    for (double p : prices)
    {
        auto m = std::make_shared<market_event>(t, "X", p, p, p, p);
        deliver_valid_event(a, m);
        t += period;
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
    EXPECT_TRUE(report.sharpe_ratio_valid);
    EXPECT_EQ(report.sharpe_ratio_reason,
              "computed_at_configured_cadence");
    EXPECT_NEAR(report.sharpe_ratio, expected_sharpe, 1e-9);
    EXPECT_GT(report.sharpe_ratio, 0.0);
    // The rolling window contains this complete, non-flat return series.
    // Zero would mean the export path failed to receive market returns.
    EXPECT_NE(report.rolling_sharpe, 0.0);
    EXPECT_GT(report.rolling_max_drawdown, 0.0);
    EXPECT_EQ(report.rolling_sharpe_reason, "computed");
    EXPECT_EQ(report.rolling_max_drawdown_reason, "computed");
}

TEST(Analytics, SharpeFailsClosedWhenEventDensityDoesNotMatchCadence)
{
    const auto run = [](bool add_intermediate_mark) {
        Analytics analytics(1'000.0, 16, 0.0, 365);
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(0), "A", 100.0, 100.0, 100.0, 100.0));
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(0), "A", 1, order_side::buy,
            1.0, 100.0, 0.0, 0.0, 1));
        if (add_intermediate_mark)
            deliver_valid_event(analytics, std::make_shared<market_event>(
                std::chrono::system_clock::time_point{std::chrono::days{182}},
                "A", 105.0, 105.0, 105.0, 105.0));
        deliver_valid_event(analytics, std::make_shared<market_event>(
            std::chrono::system_clock::time_point{std::chrono::days{365}},
            "A", 110.0, 110.0, 110.0, 110.0));
        return analytics.generate_report();
    };

    const auto sparse = run(false);
    const auto dense = run(true);
    EXPECT_FALSE(sparse.sharpe_ratio_valid);
    EXPECT_FALSE(dense.sharpe_ratio_valid);
    EXPECT_EQ(sparse.sharpe_ratio_reason, "observation_interval_mismatch");
    EXPECT_EQ(dense.sharpe_ratio_reason, "observation_interval_mismatch");
    EXPECT_DOUBLE_EQ(sparse.sharpe_ratio, 0.0);
    EXPECT_DOUBLE_EQ(dense.sharpe_ratio, 0.0);
}

TEST(Analytics, AnnualizedReturn_MatchesCompoundingFormula)
{
    // Eight elapsed calendar days use the causal wall-time horizon. The
    // configured ppy remains the sampling contract for Sharpe, not a
    // substitute for elapsed time in CAGR.
    const std::size_t ppy = 252;
    Analytics a(100000.0, 252, 0.0, ppy);

    auto mkt0 = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    deliver_valid_event(a, mkt0);
    auto bo = std::make_shared<order_event>(epoch_ms(0), "X", order_type::limit, order_side::buy, 100, 100.0);
    bo->set_order_id(1);
    deliver_valid_event(a, bo);
    auto bf = std::make_shared<fill_event>(epoch_ms(0), "X", 1, order_side::buy, 100, 100.0, 0.0);
    deliver_valid_event(a, bf);

    const std::vector<double> prices = {101.0, 102.0, 103.0, 104.0, 105.0, 106.0, 107.0, 108.0};
    std::int64_t day = 1;
    for (double p : prices)
    {
        auto m = std::make_shared<market_event>(
            std::chrono::system_clock::time_point{std::chrono::days{day++}},
            "X", p, p, p, p);
        deliver_valid_event(a, m);
    }

    auto r = a.generate_report();
    double expected_ann = std::pow(1.0 + r.cumulative_return,
                                    365.0 / static_cast<double>(prices.size())) - 1.0;
    EXPECT_TRUE(r.annualized_return_valid);
    EXPECT_EQ(r.annualized_return_reason,
              "computed_from_causal_elapsed_time");
    EXPECT_NEAR(r.annualized_return, expected_ann, 1e-12);
}

TEST(Analytics, RunningDrawdown_MatchesPostHoc)
{
    Analytics a(50000.0);

    // Simulate equity curve: 50000 -> 55000 -> 52000 -> 58000 -> 51000
    // Max drawdown should be (58000 - 51000) / 58000
    auto mkt1 = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    deliver_valid_event(a, mkt1);

    // Buy 100 shares at 100
    auto bo = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 100, 100.0);
    bo->set_order_id(1);
    deliver_valid_event(a, bo);
    auto bf = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 100, 100.0, 0.0);
    deliver_valid_event(a, bf);
    // cash = 50000 - 10000 = 40000, position = 100 shares

    // Market at 150: equity = 40000 + 100*150 = 55000
    auto mkt2 = std::make_shared<market_event>(epoch_ms(2), "X", 150, 150, 150, 150.0);
    deliver_valid_event(a, mkt2);

    // Market at 120: equity = 40000 + 100*120 = 52000
    auto mkt3 = std::make_shared<market_event>(epoch_ms(3), "X", 120, 120, 120, 120.0);
    deliver_valid_event(a, mkt3);

    // Market at 180: equity = 40000 + 100*180 = 58000
    auto mkt4 = std::make_shared<market_event>(epoch_ms(4), "X", 180, 180, 180, 180.0);
    deliver_valid_event(a, mkt4);

    // Market at 110: equity = 40000 + 100*110 = 51000
    auto mkt5 = std::make_shared<market_event>(epoch_ms(5), "X", 110, 110, 110, 110.0);
    deliver_valid_event(a, mkt5);

    // Post-hoc: peak = 58000, trough = 51000, dd = (58000-51000)/58000
    double expected_dd = (58000.0 - 51000.0) / 58000.0 * 100.0;

    auto r = a.generate_report();
    EXPECT_NEAR(r.max_drawdown, expected_dd, 0.01);
}

TEST(Analytics, DrawdownWitnessRemainsExactWhenCurveIsDecimated)
{
    // The five points overflow the four-point export cap. The retained chart
    // intentionally contains only t=0,2,4, so neither true extremum remains.
    Analytics a(1000.0, 252, 0.0, 525600, 4);
    deliver_valid_event(a, std::make_shared<market_event>(
        epoch_ms(0), "X", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(a, std::make_shared<fill_event>(
        epoch_ms(0), "X", 1, order_side::buy, 1.0, 100.0, 0.0, 0.0, 1));
    int64_t ts = 1;
    for (const double price : {200.0, 100.0, 50.0, 100.0})
    {
        deliver_valid_event(a, std::make_shared<market_event>(
            epoch_ms(ts++), "X", price, price, price, price));
    }

    const auto r = a.generate_report();
    ASSERT_EQ(r.equity_curve_sample_stride, 2u);
    ASSERT_EQ(r.benchmark_equity_curve_sample_stride, 2u);
    EXPECT_EQ(r.benchmark_curve_observation_basis,
              "selected_symbol_market_marks");
    ASSERT_EQ(r.equity_curve.size(), 3u);
    EXPECT_NEAR(r.max_drawdown_peak_equity, 1100.0, 1e-12);
    EXPECT_NEAR(r.max_drawdown_trough_equity, 950.0, 1e-12);
    const double witness_drawdown =
        (r.max_drawdown_peak_equity - r.max_drawdown_trough_equity)
        / r.max_drawdown_peak_equity * 100.0;
    EXPECT_NEAR(r.max_drawdown, witness_drawdown, 1e-12);
    EXPECT_GT(r.max_drawdown, 13.0);
}

TEST(Analytics, Snapshot_MidRun_ReturnsPartialMetrics)
{
    Analytics a;
    auto mkt = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    deliver_valid_event(a, mkt);

    // Do one winning trade
    auto bo = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 10, 100.0);
    bo->set_order_id(1);
    deliver_valid_event(a, bo);
    auto bf = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 10, 100.0, 0.0);
    deliver_valid_event(a, bf);

    auto mkt2 = std::make_shared<market_event>(epoch_ms(2), "X", 110, 110, 110, 110.0);
    deliver_valid_event(a, mkt2);

    auto so = std::make_shared<order_event>(epoch_ms(3), "X", order_type::limit, order_side::sell, 10, 110.0);
    so->set_order_id(2);
    deliver_valid_event(a, so);
    auto sf = std::make_shared<fill_event>(epoch_ms(3), "X", 2, order_side::sell, 10, 110.0, 0.0);
    deliver_valid_event(a, sf);

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
    deliver_valid_event(a, mkt);

    // Sell 10 @ 100 to open short
    auto so = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::sell, 10, 100.0);
    so->set_order_id(1);
    deliver_valid_event(a, so);
    auto sf = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::sell, 10, 100.0, 0.0);
    deliver_valid_event(a, sf);

    // Buy 10 @ 80 to close short -> profit 10 * (100 - 80) = 200
    auto bo = std::make_shared<order_event>(epoch_ms(2), "X", order_type::limit, order_side::buy, 10, 80.0);
    bo->set_order_id(2);
    deliver_valid_event(a, bo);
    auto bf = std::make_shared<fill_event>(epoch_ms(2), "X", 2, order_side::buy, 10, 80.0, 0.0);
    deliver_valid_event(a, bf);

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
    deliver_valid_event(a, mkt);

    // Buy 10 @ 100, then buy 10 @ 120 -> avg entry = 110 on 20 units
    auto bo1 = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 10, 100.0);
    bo1->set_order_id(1);
    deliver_valid_event(a, bo1);
    auto bf1 = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 10, 100.0, 0.0);
    deliver_valid_event(a, bf1);

    auto bo2 = std::make_shared<order_event>(epoch_ms(2), "X", order_type::limit, order_side::buy, 10, 120.0);
    bo2->set_order_id(2);
    deliver_valid_event(a, bo2);
    auto bf2 = std::make_shared<fill_event>(epoch_ms(2), "X", 2, order_side::buy, 10, 120.0, 0.0);
    deliver_valid_event(a, bf2);

    // Sell 20 @ 130 -> pnl = 20 * (130 - 110) = 400
    auto so = std::make_shared<order_event>(epoch_ms(3), "X", order_type::limit, order_side::sell, 20, 130.0);
    so->set_order_id(3);
    deliver_valid_event(a, so);
    auto sf = std::make_shared<fill_event>(epoch_ms(3), "X", 3, order_side::sell, 20, 130.0, 0.0);
    deliver_valid_event(a, sf);

    auto r = a.generate_report();
    EXPECT_EQ(r.total_trades, 1u);
    ASSERT_EQ(r.trade_returns.size(), 1u);
    EXPECT_NEAR(r.trade_returns[0], 400.0, 1e-9);
}

TEST(Analytics, Flipping_LongToShortInOneFill)
{
    Analytics a;
    auto mkt = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    deliver_valid_event(a, mkt);

    // Buy 10 @ 100 -> long 10
    auto bo = std::make_shared<order_event>(epoch_ms(1), "X", order_type::limit, order_side::buy, 10, 100.0);
    bo->set_order_id(1);
    deliver_valid_event(a, bo);
    auto bf = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 10, 100.0, 0.0);
    deliver_valid_event(a, bf);

    // Sell 15 @ 120 -> closes 10 (pnl = 10 * 20 = 200), opens short 5 @ 120
    auto so = std::make_shared<order_event>(epoch_ms(2), "X", order_type::limit, order_side::sell, 15, 120.0);
    so->set_order_id(2);
    deliver_valid_event(a, so);
    auto sf = std::make_shared<fill_event>(epoch_ms(2), "X", 2, order_side::sell, 15, 120.0, 0.0);
    deliver_valid_event(a, sf);

    auto r = a.generate_report();
    // One closed round-trip so far
    EXPECT_EQ(r.total_trades, 1u);
    ASSERT_EQ(r.trade_returns.size(), 1u);
    EXPECT_NEAR(r.trade_returns[0], 200.0, 1e-9);

    // Buy 5 @ 100 -> closes short (pnl = 5 * (120 - 100) = 100)
    auto bo2 = std::make_shared<order_event>(epoch_ms(3), "X", order_type::limit, order_side::buy, 5, 100.0);
    bo2->set_order_id(3);
    deliver_valid_event(a, bo2);
    auto bf2 = std::make_shared<fill_event>(epoch_ms(3), "X", 3, order_side::buy, 5, 100.0, 0.0);
    deliver_valid_event(a, bf2);

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
    deliver_valid_event(a, mkt);

    // Long 10 A @ 100
    auto bf_a = std::make_shared<fill_event>(epoch_ms(1), "A", 1, order_side::buy, 10, 100.0, 0.0);
    deliver_valid_event(a, bf_a);
    // Short 10 B @ 50 — must OPEN a short on B, not close the A long.
    auto sf_b = std::make_shared<fill_event>(epoch_ms(2), "B", 2, order_side::sell, 10, 50.0, 0.0);
    deliver_valid_event(a, sf_b);

    auto r0 = a.generate_report();
    EXPECT_EQ(r0.total_trades, 0u) << "cross-symbol fill must not be booked as a close";

    // Close A @ 110 → +100; close B @ 40 → +100.
    auto sf_a = std::make_shared<fill_event>(epoch_ms(3), "A", 3, order_side::sell, 10, 110.0, 0.0);
    deliver_valid_event(a, sf_a);
    auto bf_b = std::make_shared<fill_event>(epoch_ms(4), "B", 4, order_side::buy, 10, 40.0, 0.0);
    deliver_valid_event(a, bf_b);

    auto r = a.generate_report();
    EXPECT_EQ(r.total_trades, 2u);
    ASSERT_EQ(r.trade_returns.size(), 2u);
    EXPECT_NEAR(r.trade_returns[0], 100.0, 1e-9);
    EXPECT_NEAR(r.trade_returns[1], 100.0, 1e-9);
    EXPECT_NEAR(r.per_symbol.at("A").total_pnl, 100.0, 1e-9);
    EXPECT_NEAR(r.per_symbol.at("B").total_pnl, 100.0, 1e-9);
}

TEST(Analytics, MultiSymbol_FinalEquityUsesLatestMarksButCurveWaitsForSync)
{
    Analytics a;
    auto m_a = std::make_shared<market_event>(epoch_ms(0), "A", 100, 100, 100, 100.0);
    deliver_valid_event(a, m_a);
    auto m_b = std::make_shared<market_event>(epoch_ms(1), "B", 10, 10, 10, 10.0);
    deliver_valid_event(a, m_b);

    // Long 10 A @ 100 and 10 B @ 10 → cash = 100000 - 1000 - 100 = 98900
    auto bf_a = std::make_shared<fill_event>(epoch_ms(2), "A", 1, order_side::buy, 10, 100.0, 0.0);
    deliver_valid_event(a, bf_a);
    auto bf_b = std::make_shared<fill_event>(epoch_ms(3), "B", 2, order_side::buy, 10, 10.0, 0.0);
    deliver_valid_event(a, bf_b);

    // A moves to 110, B stays 10: equity = 98900 + 1100 + 100 = 100100.
    auto m_a2 = std::make_shared<market_event>(epoch_ms(4), "A", 110, 110, 110, 110.0);
    deliver_valid_event(a, m_a2);

    const auto partial = a.generate_report();
    EXPECT_NEAR(partial.final_equity, 100100.0, 1e-6);
    ASSERT_FALSE(partial.equity_curve.empty());
    EXPECT_NEAR(partial.equity_curve.back().equity, 100000.0, 1e-6)
        << "a half-marked multi-symbol portfolio is not a causal return point";

    auto m_b2 = std::make_shared<market_event>(epoch_ms(4), "B", 10, 10, 10, 10.0);
    deliver_valid_event(a, m_b2);
    const auto synchronized = a.generate_report();
    EXPECT_NEAR(synchronized.final_equity, 100100.0, 1e-6);
    EXPECT_NEAR(synchronized.equity_curve.back().equity, 100100.0, 1e-6);
}

TEST(Analytics, MultiSymbolPortfolioClockUsesNewestIncludedMarkRegardlessOfArrival)
{
    const auto run = [](bool newest_arrives_first) {
        Analytics analytics;
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(0), "A", 100.0, 100.0, 100.0, 100.0));
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(0), "B", 100.0, 100.0, 100.0, 100.0));
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(1), "A", 1, order_side::buy,
            1.0, 100.0, 0.0, 0.0, 1));
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(2), "B", 2, order_side::buy,
            1.0, 100.0, 0.0, 0.0, 2));

        const auto mark_a = std::make_shared<market_event>(
            epoch_ms(100), "A", 110.0, 110.0, 110.0, 110.0);
        const auto mark_b = std::make_shared<market_event>(
            epoch_ms(101), "B", 120.0, 120.0, 120.0, 120.0);
        if (newest_arrives_first)
        {
            deliver_valid_event(analytics, mark_b);
            deliver_valid_event(analytics, mark_a);
        }
        else
        {
            deliver_valid_event(analytics, mark_a);
            deliver_valid_event(analytics, mark_b);
        }
        return analytics.generate_report();
    };

    const auto chronological = run(false);
    const auto permuted = run(true);
    ASSERT_FALSE(chronological.equity_curve.empty());
    ASSERT_FALSE(permuted.equity_curve.empty());
    EXPECT_EQ(chronological.equity_curve.back().timestamp, epoch_ms(101));
    EXPECT_EQ(permuted.equity_curve.back().timestamp, epoch_ms(101));
    EXPECT_DOUBLE_EQ(permuted.equity_curve.back().equity,
                     chronological.equity_curve.back().equity);
}

TEST(Analytics,
     C15_ConstantExtraSymbolDoesNotChangePortfolioPerformanceMetrics)
{
    const auto run = [](bool add_constant_symbol) {
        Analytics analytics(/*initial_cash=*/100'000.0,
                            /*rolling_window=*/16,
                            /*risk_free_rate=*/0.0,
                            /*periods_per_year=*/252);
        const auto mark = [&](std::int64_t ts_ms,
                              const std::string& symbol,
                              double price) {
            deliver_valid_event(analytics, std::make_shared<market_event>(
                epoch_ms(ts_ms), symbol, price, price, price, price));
        };

        mark(0, "A", 100.0);
        if (add_constant_symbol)
            mark(0, "0_CONSTANT", 10.0);
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(1), "A", 1, order_side::buy,
            100.0, 100.0, 0.0, 0.0, 1));

        for (const auto& [ts_ms, price] :
             std::vector<std::pair<std::int64_t, double>>{
                 {60'000, 102.0}, {120'000, 101.0}, {180'000, 105.0}})
        {
            mark(ts_ms, "A", price);
            if (add_constant_symbol)
                mark(ts_ms, "0_CONSTANT", 10.0);
        }
        return analytics.generate_report();
    };

    const auto baseline = run(false);
    const auto augmented = run(true);

    ASSERT_GT(baseline.rolling_return_count, 1u);
    EXPECT_DOUBLE_EQ(augmented.final_equity, baseline.final_equity);
    EXPECT_DOUBLE_EQ(augmented.cumulative_return,
                     baseline.cumulative_return);
    EXPECT_EQ(augmented.rolling_return_count,
              baseline.rolling_return_count);
    EXPECT_DOUBLE_EQ(augmented.annualized_return,
                     baseline.annualized_return);
    EXPECT_DOUBLE_EQ(augmented.sharpe_ratio, baseline.sharpe_ratio);
    EXPECT_DOUBLE_EQ(augmented.sortino_ratio, baseline.sortino_ratio);
    EXPECT_DOUBLE_EQ(augmented.rolling_sharpe,
                     baseline.rolling_sharpe);
    EXPECT_DOUBLE_EQ(augmented.max_drawdown, baseline.max_drawdown);
    EXPECT_DOUBLE_EQ(augmented.time_in_market_pct,
                     baseline.time_in_market_pct);
    EXPECT_DOUBLE_EQ(augmented.buy_and_hold_return,
                     baseline.buy_and_hold_return);
    EXPECT_DOUBLE_EQ(augmented.strategy_vs_benchmark,
                     baseline.strategy_vs_benchmark);
    ASSERT_EQ(augmented.benchmark_equity_curve.size(),
              baseline.benchmark_equity_curve.size());
    for (std::size_t i = 0; i < baseline.benchmark_equity_curve.size(); ++i)
    {
        EXPECT_EQ(augmented.benchmark_equity_curve[i].timestamp,
                  baseline.benchmark_equity_curve[i].timestamp);
        EXPECT_DOUBLE_EQ(augmented.benchmark_equity_curve[i].equity,
                         baseline.benchmark_equity_curve[i].equity);
    }
}

TEST(Analytics, FlatHistoricalSymbolDoesNotMasqueradeAsTwoOpenPositions)
{
    Analytics analytics;
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(0), "B", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "B", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(2), "B", 2, order_side::sell,
        1.0, 100.0, 0.0, 0.0, 2));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(3), "A", 50.0, 50.0, 50.0, 50.0));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(4), "A", 3, order_side::buy,
        1.0, 50.0, 0.0, 0.0, 3));

    const auto report = analytics.generate_report();
    ASSERT_EQ(report.open_positions.size(), 1u);
    EXPECT_EQ(report.open_positions.front().symbol, "A");
    EXPECT_FALSE(report.benchmark_valid);
    EXPECT_EQ(report.benchmark_reason,
              "explicit_benchmark_required_for_cross_symbol_economic_history");
}

TEST(Analytics,
     C15_OffGridConstantSymbolDoesNotCreatePortfolioReturnPeriods)
{
    const auto run = [](bool add_off_grid_marks) {
        Analytics analytics(/*initial_cash=*/100'000.0,
                            /*rolling_window=*/16,
                            /*risk_free_rate=*/0.0,
                            /*periods_per_year=*/252);
        const auto mark = [&](std::int64_t ts_ms,
                              const std::string& symbol,
                              double price) {
            deliver_valid_event(analytics, std::make_shared<market_event>(
                epoch_ms(ts_ms), symbol, price, price, price, price));
        };

        mark(0, "A", 100.0);
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(1), "A", 1, order_side::buy,
            100.0, 100.0, 0.0, 0.0, 1));
        if (add_off_grid_marks)
            mark(30'000, "CONSTANT", 10.0);
        mark(60'000, "A", 102.0);
        if (add_off_grid_marks)
            mark(90'000, "CONSTANT", 10.0);
        mark(120'000, "A", 101.0);

        return analytics.generate_report();
    };

    const auto baseline = run(false);
    const auto augmented = run(true);
    EXPECT_DOUBLE_EQ(augmented.final_equity, baseline.final_equity);
    EXPECT_EQ(augmented.rolling_return_count,
              baseline.rolling_return_count);
    EXPECT_DOUBLE_EQ(augmented.annualized_return,
                     baseline.annualized_return);
    EXPECT_DOUBLE_EQ(augmented.sharpe_ratio, baseline.sharpe_ratio);
    EXPECT_DOUBLE_EQ(augmented.sortino_ratio, baseline.sortino_ratio);
    EXPECT_DOUBLE_EQ(augmented.time_in_market_pct,
                     baseline.time_in_market_pct);
}

TEST(Analytics, C15_DuplicateSameTimestampMarkDoesNotAddReturnPeriod)
{
    Analytics analytics(/*initial_cash=*/1'000.0,
                        /*rolling_window=*/16,
                        /*risk_free_rate=*/0.0,
                        /*periods_per_year=*/252);
    const auto mark = [&](std::int64_t ts_ms, double price) {
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(ts_ms), "A", price, price, price, price));
    };

    mark(0, 100.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    mark(60'000, 101.0);
    const auto before = analytics.generate_report();
    mark(60'000, 101.0);
    const auto after = analytics.generate_report();

    EXPECT_EQ(after.rolling_return_count, before.rolling_return_count);
    EXPECT_DOUBLE_EQ(after.sharpe_ratio, before.sharpe_ratio);
    EXPECT_DOUBLE_EQ(after.sortino_ratio, before.sortino_ratio);
}

TEST(Analytics,
     C15_SimultaneousPortfolioMarksArePermutationInvariant)
{
    const auto run = [](bool mark_long_symbol_first) {
        Analytics analytics(/*initial_cash=*/1'000.0,
                            /*rolling_window=*/16,
                            /*risk_free_rate=*/0.0,
                            /*periods_per_year=*/252);
        const auto mark = [&](std::int64_t ts_ms,
                              const std::string& symbol,
                              double price) {
            deliver_valid_event(analytics, std::make_shared<market_event>(
                epoch_ms(ts_ms), symbol, price, price, price, price));
        };

        mark(0, "A", 100.0);
        mark(0, "B", 100.0);
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(1), "A", 1, order_side::buy,
            1.0, 100.0, 0.0, 0.0, 1));
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(1), "B", 2, order_side::sell,
            1.0, 100.0, 0.0, 0.0, 2));

        if (mark_long_symbol_first)
        {
            mark(60'000, "A", 200.0);
            mark(60'000, "B", 200.0);
        }
        else
        {
            mark(60'000, "B", 200.0);
            mark(60'000, "A", 200.0);
        }
        return analytics.generate_report();
    };

    const auto long_first = run(true);
    const auto short_first = run(false);
    ASSERT_DOUBLE_EQ(long_first.final_equity, short_first.final_equity);
    EXPECT_DOUBLE_EQ(long_first.cumulative_return,
                     short_first.cumulative_return);
    EXPECT_EQ(long_first.rolling_return_count,
              short_first.rolling_return_count);
    EXPECT_DOUBLE_EQ(long_first.sharpe_ratio, short_first.sharpe_ratio);
    EXPECT_DOUBLE_EQ(long_first.sortino_ratio, short_first.sortino_ratio);
    EXPECT_DOUBLE_EQ(long_first.max_drawdown, short_first.max_drawdown);
    EXPECT_DOUBLE_EQ(long_first.max_drawdown_peak_equity,
                     short_first.max_drawdown_peak_equity);
    EXPECT_DOUBLE_EQ(long_first.max_drawdown_trough_equity,
                     short_first.max_drawdown_trough_equity);
}

TEST(Analytics, C15_AsynchronousMarksAdvanceCompletePortfolioCycles)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(2), "B", 2, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 2));

    const auto mark = [&](std::int64_t ts, const std::string& symbol,
                          double price) {
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(ts), symbol, price, price, price, price));
    };
    mark(100, "A", 110.0);
    mark(101, "B", 120.0);
    mark(200, "A", 120.0);
    mark(201, "B", 130.0);

    const auto report = analytics.generate_report();
    ASSERT_EQ(report.equity_curve.size(), 2u);
    EXPECT_EQ(report.rolling_return_count, 1u);
    EXPECT_DOUBLE_EQ(report.equity_curve.front().equity, 1'030.0);
    EXPECT_DOUBLE_EQ(report.equity_curve.back().equity, 1'050.0);
    EXPECT_DOUBLE_EQ(report.final_equity, 1'050.0);
    EXPECT_TRUE(report.time_in_market_valid);
    EXPECT_DOUBLE_EQ(report.time_in_market_pct, 100.0);
}

TEST(Analytics, C15_RepeatedSymbolBeforeCycleCompletionFailsClosed)
{
    const auto run = [](bool globally_ordered) {
        Analytics analytics(1'000.0);
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(1), "A", 1, order_side::buy,
            1.0, 100.0, 0.0, 0.0, 1));
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(1), "B", 2, order_side::buy,
            1.0, 100.0, 0.0, 0.0, 2));
        const auto mark = [&](std::int64_t ts, const char* symbol,
                              double price) {
            deliver_valid_event(analytics, std::make_shared<market_event>(
                epoch_ms(ts), symbol, price, price, price, price));
        };
        if (globally_ordered)
        {
            mark(100, "A", 110.0);
            mark(150, "B", 110.0);
            mark(200, "A", 120.0);
            mark(250, "B", 120.0);
        }
        else
        {
            mark(100, "A", 110.0);
            mark(200, "A", 120.0);
            mark(150, "B", 110.0);
            mark(250, "B", 120.0);
        }
        return analytics.generate_report();
    };

    const auto ordered = run(true);
    const auto ambiguous = run(false);
    ASSERT_TRUE(ordered.portfolio_time_series_valid);
    ASSERT_EQ(ordered.equity_curve.size(), 2u);
    EXPECT_DOUBLE_EQ(ordered.equity_curve[0].equity, 1'020.0);
    EXPECT_DOUBLE_EQ(ordered.equity_curve[1].equity, 1'040.0);
    EXPECT_FALSE(ambiguous.portfolio_time_series_valid);
    EXPECT_EQ(ambiguous.portfolio_time_series_reason,
              "ambiguous_cross_symbol_arrival_without_watermark");
    EXPECT_EQ(ambiguous.ambiguous_portfolio_mark_sequences_rejected, 1u);
    EXPECT_FALSE(ambiguous.sharpe_ratio_valid);
    EXPECT_FALSE(ambiguous.sortino_ratio_valid);
    EXPECT_FALSE(ambiguous.annualized_return_valid);
    EXPECT_DOUBLE_EQ(ambiguous.final_equity, ordered.final_equity);
}

TEST(Analytics, C15_IncompleteFinalPortfolioMarkCycleFailsClosed)
{
    constexpr std::int64_t day_ms = 86'400'000;
    Analytics analytics(1'000.0);
    const auto mark = [&](std::int64_t timestamp_ms, const char* symbol,
                          double price) {
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(timestamp_ms), symbol, price, price, price, price));
    };

    mark(day_ms, "A", 100.0);
    mark(day_ms, "B", 100.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(day_ms + 1), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(day_ms + 1), "B", 2, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 2));

    mark(2 * day_ms, "A", 100.0);
    mark(2 * day_ms, "B", 100.0);
    mark(367 * day_ms, "A", 110.0);

    const auto report = analytics.generate_report();
    EXPECT_DOUBLE_EQ(report.final_equity, 1'010.0);
    ASSERT_FALSE(report.equity_curve.empty());
    EXPECT_DOUBLE_EQ(report.equity_curve.back().equity, 1'000.0);
    EXPECT_FALSE(report.portfolio_time_series_valid);
    EXPECT_EQ(report.portfolio_time_series_reason,
              "incomplete_portfolio_mark_cycle_at_snapshot");
    EXPECT_FALSE(report.valuation_complete);
    EXPECT_EQ(report.valuation_reason,
              "incomplete_portfolio_mark_cycle_at_snapshot");
    EXPECT_FALSE(report.annualized_return_valid);
    EXPECT_FALSE(report.sharpe_ratio_valid);
    EXPECT_FALSE(report.sortino_ratio_valid);
}

TEST(Analytics,
     C15_UnrelatedSymbolSpreadCannotBypassCandidateSpreadRisk)
{
    Analytics analytics(100'000.0);
    const l2_level wide_bids[] = {{99.0, 10}};
    const l2_level wide_asks[] = {{101.0, 10}};
    deliver_valid_event(analytics, std::make_shared<l2_snapshot_event>(
        epoch_ms(1), "A", wide_bids, std::size(wide_bids),
        wide_asks, std::size(wide_asks)));

    const l2_level narrow_bids[] = {{99.95, 10}};
    const l2_level narrow_asks[] = {{100.05, 10}};
    deliver_valid_event(analytics, std::make_shared<l2_snapshot_event>(
        epoch_ms(2), "B", narrow_bids, std::size(narrow_bids),
        narrow_asks, std::size(narrow_asks)));

    risk_limits limits;
    limits.max_spread_bps = 50.0;
    RiskManager risk(limits);
    portfolio account(100'000.0);
    order_event candidate(
        epoch_ms(3), "A", order_type::limit, order_side::buy,
        1.0, 100.0);

    EXPECT_EQ(risk.check_order(candidate, account, analytics.risk_view()),
              risk_action::halt)
        << "A's 200 bps spread is a severe breach and must not be replaced "
           "by B's later 10 bps snapshot";
}

TEST(Analytics, C15_ThreadedMarksCoalesceCompletePortfolioSnapshot)
{
    const auto run = [](bool long_symbol_first) {
        Analytics analytics(1'000.0);
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(1), "A", 1, order_side::buy,
            1.0, 100.0, 0.0, 0.0, 1));
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(1), "B", 2, order_side::sell,
            1.0, 100.0, 0.0, 0.0, 2));

        if (long_symbol_first)
        {
            analytics.on_mark("A", 200.0);
            analytics.on_mark("B", 200.0);
        }
        else
        {
            analytics.on_mark("B", 200.0);
            analytics.on_mark("A", 200.0);
        }
        return analytics.generate_report();
    };

    const auto long_first = run(true);
    const auto short_first = run(false);
    EXPECT_DOUBLE_EQ(long_first.final_equity, 1'000.0);
    EXPECT_DOUBLE_EQ(short_first.final_equity, 1'000.0);
    EXPECT_DOUBLE_EQ(long_first.max_drawdown, 0.0);
    EXPECT_DOUBLE_EQ(short_first.max_drawdown, 0.0);
    EXPECT_DOUBLE_EQ(long_first.max_drawdown,
                     short_first.max_drawdown);
}

TEST(Analytics, C15_ImplicitBenchmarkFailsClosedForMultiSymbolMarket)
{
    const auto run = [](bool a_first) {
        Analytics analytics(1'000.0);
        const auto mark = [&](std::int64_t ts_ms,
                              const std::string& symbol,
                              double price) {
            deliver_valid_event(analytics, std::make_shared<market_event>(
                epoch_ms(ts_ms), symbol, price, price, price, price));
        };
        if (a_first)
        {
            mark(0, "A", 100.0);
            mark(0, "B", 100.0);
        }
        else
        {
            mark(0, "B", 100.0);
            mark(0, "A", 100.0);
        }
        if (a_first)
        {
            mark(60'000, "A", 120.0);
            mark(60'000, "B", 25.0);
        }
        else
        {
            mark(60'000, "B", 25.0);
            mark(60'000, "A", 120.0);
        }
        return analytics.generate_report();
    };

    const auto a_first = run(true);
    const auto b_first = run(false);
    EXPECT_FALSE(a_first.benchmark_valid);
    EXPECT_FALSE(b_first.benchmark_valid);
    EXPECT_TRUE(a_first.benchmark_symbol.empty());
    EXPECT_TRUE(b_first.benchmark_symbol.empty());
    EXPECT_EQ(a_first.benchmark_reason,
              "explicit_benchmark_required_for_multi_symbol_market");
    EXPECT_EQ(b_first.benchmark_reason, a_first.benchmark_reason);
    EXPECT_TRUE(a_first.benchmark_equity_curve.empty());
    EXPECT_TRUE(b_first.benchmark_equity_curve.empty());
}

TEST(Analytics, C15_LexicographicallyEarlierConstantCannotBecomeBenchmark)
{
    const auto run = [](bool add_constant, bool constant_first = false) {
        Analytics analytics(1'000.0);
        const auto mark = [&](std::int64_t ts, const std::string& symbol,
                              double price) {
            deliver_valid_event(analytics, std::make_shared<market_event>(
                epoch_ms(ts), symbol, price, price, price, price));
        };
        if (add_constant && constant_first)
            mark(0, "0_CONSTANT", 10.0);
        mark(0, "A", 100.0);
        if (add_constant && !constant_first)
            mark(0, "0_CONSTANT", 10.0);
        if (add_constant && constant_first)
            mark(60'000, "0_CONSTANT", 10.0);
        mark(60'000, "A", 120.0);
        if (add_constant && !constant_first)
            mark(60'000, "0_CONSTANT", 10.0);
        return analytics.generate_report();
    };

    const auto baseline = run(false);
    const auto augmented = run(true);
    const auto constant_first = run(true, true);
    ASSERT_TRUE(baseline.benchmark_valid);
    EXPECT_EQ(baseline.benchmark_symbol, "A");
    EXPECT_DOUBLE_EQ(baseline.buy_and_hold_return, 0.20);
    EXPECT_TRUE(augmented.benchmark_valid);
    EXPECT_EQ(augmented.benchmark_symbol, "A");
    EXPECT_DOUBLE_EQ(augmented.buy_and_hold_return,
                     baseline.buy_and_hold_return);
    EXPECT_DOUBLE_EQ(augmented.strategy_vs_benchmark,
                     baseline.strategy_vs_benchmark);
    EXPECT_EQ(augmented.benchmark_reason,
              "single_varying_market_symbol_constant_others");
    ASSERT_EQ(augmented.benchmark_equity_curve.size(), 2u);
    EXPECT_DOUBLE_EQ(augmented.benchmark_equity_curve.front().equity,
                     1'000.0);
    EXPECT_DOUBLE_EQ(augmented.benchmark_equity_curve.back().equity,
                     1'200.0);
    EXPECT_TRUE(constant_first.benchmark_valid);
    EXPECT_EQ(constant_first.benchmark_symbol, "A");
    EXPECT_DOUBLE_EQ(constant_first.buy_and_hold_return,
                     baseline.buy_and_hold_return);
    EXPECT_DOUBLE_EQ(constant_first.strategy_vs_benchmark,
                     baseline.strategy_vs_benchmark);
}

TEST(Analytics, C15_CausalBenchmarkRetainsIntermediateDrawdownPath)
{
    Analytics analytics(1'000.0);
    const auto mark = [&](std::int64_t ts, const std::string& symbol,
                          double price) {
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(ts), symbol, price, price, price, price));
    };

    mark(1, "0_CONSTANT", 10.0);
    mark(2, "A", 100.0);
    mark(3, "A", 50.0);
    mark(4, "A", 120.0);

    const auto report = analytics.generate_report();
    ASSERT_TRUE(report.benchmark_valid);
    EXPECT_EQ(report.benchmark_symbol, "A");
    EXPECT_EQ(report.benchmark_reason,
              "single_varying_market_symbol_constant_others");
    ASSERT_EQ(report.benchmark_equity_curve.size(), 3u);
    EXPECT_EQ(report.benchmark_equity_curve[0].timestamp, epoch_ms(2));
    EXPECT_DOUBLE_EQ(report.benchmark_equity_curve[0].equity, 1'000.0);
    EXPECT_EQ(report.benchmark_equity_curve[1].timestamp, epoch_ms(3));
    EXPECT_DOUBLE_EQ(report.benchmark_equity_curve[1].equity, 500.0);
    EXPECT_EQ(report.benchmark_equity_curve[2].timestamp, epoch_ms(4));
    EXPECT_DOUBLE_EQ(report.benchmark_equity_curve[2].equity, 1'200.0);
}

TEST(Analytics, C15_HistoricalVariationCannotDisappearAtStartingPrice)
{
    Analytics analytics(1'000.0);
    const auto mark = [&](std::int64_t ts, const std::string& symbol,
                          double price) {
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(ts), symbol, price, price, price, price));
    };

    mark(1, "A", 100.0);
    mark(2, "A", 150.0);
    mark(3, "A", 100.0);
    mark(4, "B", 100.0);
    mark(5, "B", 200.0);

    const auto report = analytics.generate_report();
    EXPECT_FALSE(report.benchmark_valid);
    EXPECT_TRUE(report.benchmark_symbol.empty());
    EXPECT_EQ(report.benchmark_reason,
              "explicit_benchmark_required_for_multi_symbol_market");
    EXPECT_TRUE(report.benchmark_equity_curve.empty());
}

TEST(Analytics, C15_CausalBenchmarkPromotionIsTimestampPermutationInvariant)
{
    const auto run = [](bool constant_first_at_promotion) {
        Analytics analytics(1'000.0);
        const auto mark = [&](std::int64_t ts, const std::string& symbol,
                              double price) {
            deliver_valid_event(analytics, std::make_shared<market_event>(
                epoch_ms(ts), symbol, price, price, price, price));
        };
        mark(1, "0_CONSTANT", 10.0);
        mark(1, "A", 100.0);
        if (constant_first_at_promotion)
        {
            mark(2, "0_CONSTANT", 10.0);
            mark(2, "A", 50.0);
        }
        else
        {
            mark(2, "A", 50.0);
            mark(2, "0_CONSTANT", 10.0);
        }
        mark(3, "A", 120.0);
        return analytics.generate_report();
    };

    const auto constant_first = run(true);
    const auto varying_first = run(false);
    ASSERT_TRUE(constant_first.benchmark_valid);
    ASSERT_TRUE(varying_first.benchmark_valid);
    EXPECT_EQ(constant_first.benchmark_symbol, "A");
    EXPECT_EQ(varying_first.benchmark_symbol, "A");
    ASSERT_EQ(constant_first.benchmark_equity_curve.size(), 3u);
    ASSERT_EQ(varying_first.benchmark_equity_curve.size(), 3u);
    for (std::size_t i = 0;
         i < constant_first.benchmark_equity_curve.size(); ++i)
    {
        EXPECT_EQ(constant_first.benchmark_equity_curve[i].timestamp,
                  varying_first.benchmark_equity_curve[i].timestamp);
        EXPECT_DOUBLE_EQ(constant_first.benchmark_equity_curve[i].equity,
                         varying_first.benchmark_equity_curve[i].equity);
    }
    EXPECT_DOUBLE_EQ(constant_first.alpha, varying_first.alpha);
    EXPECT_DOUBLE_EQ(constant_first.beta, varying_first.beta);
    EXPECT_DOUBLE_EQ(constant_first.tracking_error,
                     varying_first.tracking_error);
    EXPECT_DOUBLE_EQ(constant_first.information_ratio,
                     varying_first.information_ratio);
}

TEST(Analytics, BenchmarkStatisticsRemainFiniteAtExtremeScale)
{
    Analytics analytics(1.0);
    const auto mark = [&](std::int64_t ts, double price) {
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(ts), "A", price, price, price, price));
    };

    const double extreme = std::numeric_limits<double>::max() / 2.0;
    mark(1, 1.0);
    mark(2, extreme);
    mark(3, 1.0);

    const auto report = analytics.generate_report();
    ASSERT_TRUE(report.benchmark_valid);
    EXPECT_EQ(report.benchmark_symbol, "A");
    EXPECT_TRUE(std::isfinite(report.alpha));
    EXPECT_TRUE(std::isfinite(report.beta));
    EXPECT_TRUE(std::isfinite(report.tracking_error));
    EXPECT_TRUE(std::isfinite(report.information_ratio));
    EXPECT_DOUBLE_EQ(report.alpha, 0.0);
    EXPECT_DOUBLE_EQ(report.beta, 0.0);
    EXPECT_GT(report.tracking_error, 0.0);
    EXPECT_NEAR(report.information_ratio,
                -1.0 / std::sqrt(2.0), 1e-12);
}

TEST(Analytics, C15_EconomicFillPreservesExistingCausalBenchmarkPath)
{
    Analytics analytics(1'000.0);
    const auto mark = [&](std::int64_t ts, double price) {
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(ts), "A", price, price, price, price));
    };
    mark(1, 100.0);
    mark(2, 50.0);
    mark(3, 120.0);
    const auto before = analytics.generate_report();

    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(4), "A", 1, order_side::buy,
        1.0, 120.0, 0.0, 0.0, 1));
    const auto after = analytics.generate_report();

    ASSERT_TRUE(after.benchmark_valid);
    EXPECT_EQ(after.benchmark_symbol, "A");
    ASSERT_EQ(after.benchmark_equity_curve.size(),
              before.benchmark_equity_curve.size());
    for (std::size_t i = 0; i < before.benchmark_equity_curve.size(); ++i)
    {
        EXPECT_EQ(after.benchmark_equity_curve[i].timestamp,
                  before.benchmark_equity_curve[i].timestamp);
        EXPECT_DOUBLE_EQ(after.benchmark_equity_curve[i].equity,
                         before.benchmark_equity_curve[i].equity);
    }
}

TEST(Analytics, C15_UnretainedPreVariationHistoryFailsClosed)
{
    Analytics analytics(1'000.0);
    const auto mark = [&](std::int64_t ts, const std::string& symbol,
                          double price) {
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(ts), symbol, price, price, price, price));
    };
    mark(1, "0_CONSTANT", 10.0);
    mark(1, "A", 100.0);
    mark(2, "A", 100.0);
    mark(3, "A", 50.0);

    const auto report = analytics.generate_report();
    EXPECT_FALSE(report.benchmark_valid);
    EXPECT_TRUE(report.benchmark_symbol.empty());
    EXPECT_EQ(report.benchmark_reason,
              "causal_benchmark_path_unavailable");
    EXPECT_TRUE(report.benchmark_equity_curve.empty());
}

TEST(Analytics, C15_TickAndL2CannotBecomeBarBenchmarkBaseline)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<tick_event>(
        epoch_ms(1), "A", 40.0, 1));
    const l2_level bids[] = {{49.0, 1}};
    const l2_level asks[] = {{51.0, 1}};
    deliver_valid_event(analytics, std::make_shared<l2_snapshot_event>(
        epoch_ms(2), "A", bids, std::size(bids),
        asks, std::size(asks)));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(3), "A", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(4), "A", 120.0, 120.0, 120.0, 120.0));

    const auto report = analytics.generate_report();
    ASSERT_TRUE(report.benchmark_valid);
    EXPECT_EQ(report.benchmark_symbol, "A");
    EXPECT_DOUBLE_EQ(report.buy_and_hold_return, 0.20);
    ASSERT_EQ(report.benchmark_equity_curve.size(), 2u);
    EXPECT_EQ(report.benchmark_equity_curve.front().timestamp, epoch_ms(3));
    EXPECT_DOUBLE_EQ(report.benchmark_equity_curve.front().equity, 1'000.0);
    EXPECT_EQ(report.benchmark_equity_curve.back().timestamp, epoch_ms(4));
    EXPECT_DOUBLE_EQ(report.benchmark_equity_curve.back().equity, 1'200.0);
}

TEST(Analytics, C15_TickOnlyBenchmarkIsExplicitlyUnsupported)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<tick_event>(
        epoch_ms(1), "A", 100.0, 1));
    deliver_valid_event(analytics, std::make_shared<tick_event>(
        epoch_ms(2), "A", 120.0, 1));

    const auto report = analytics.generate_report();
    EXPECT_FALSE(report.benchmark_valid);
    EXPECT_TRUE(report.benchmark_symbol.empty());
    EXPECT_EQ(report.benchmark_reason,
              "explicit_benchmark_required_for_multi_symbol_market");
    EXPECT_TRUE(report.benchmark_equity_curve.empty());
}

TEST(Analytics, C15_FirstNewSymbolCannotReplacePromotedBenchmark)
{
    Analytics analytics(1'000.0);
    const auto mark = [&](std::int64_t ts, const std::string& symbol,
                          double price) {
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(ts), symbol, price, price, price, price));
    };
    mark(1, "0_CONSTANT", 10.0);
    mark(1, "A", 100.0);
    mark(2, "A", 50.0);
    mark(2, "0_NEW", 10.0);
    mark(3, "A", 120.0);

    const auto report = analytics.generate_report();
    ASSERT_TRUE(report.benchmark_valid);
    EXPECT_EQ(report.benchmark_symbol, "A");
    ASSERT_EQ(report.benchmark_equity_curve.size(), 3u);
    EXPECT_DOUBLE_EQ(report.benchmark_equity_curve[0].equity, 1'000.0);
    EXPECT_DOUBLE_EQ(report.benchmark_equity_curve[1].equity, 500.0);
    EXPECT_DOUBLE_EQ(report.benchmark_equity_curve[2].equity, 1'200.0);
}

TEST(Analytics, C15_FirstNewSymbolCannotReplaceEconomicBenchmark)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(1), "0_NEW", 10.0, 10.0, 10.0, 10.0));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(2), "A", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(3), "A", 120.0, 120.0, 120.0, 120.0));

    const auto report = analytics.generate_report();
    ASSERT_TRUE(report.benchmark_valid);
    EXPECT_EQ(report.benchmark_symbol, "A");
    EXPECT_EQ(report.benchmark_reason, "single_economic_symbol");
    EXPECT_DOUBLE_EQ(report.buy_and_hold_return, 0.20);
    ASSERT_EQ(report.benchmark_equity_curve.size(), 2u);
}

TEST(Analytics, C15_LateMarketMarkCannotOverwriteNewerValuation)
{
    Analytics analytics(1'000.0);
    const auto mark = [&](std::int64_t ts_ms, double price) {
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(ts_ms), "A", price, price, price, price));
    };
    mark(100, 100.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(101), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    mark(200, 110.0);
    const auto before = analytics.generate_report();
    mark(150, 50.0);
    const auto after = analytics.generate_report();

    EXPECT_DOUBLE_EQ(before.final_equity, 1'010.0);
    EXPECT_DOUBLE_EQ(after.final_equity, before.final_equity);
    EXPECT_FALSE(after.portfolio_time_series_valid);
    EXPECT_EQ(after.portfolio_time_series_reason,
              "late_market_event_rejected");
    EXPECT_EQ(after.late_market_events_rejected, 1u);
    EXPECT_FALSE(after.annualized_return_valid);
    ASSERT_EQ(after.equity_curve.size(), before.equity_curve.size());
    for (std::size_t i = 1; i < after.equity_curve.size(); ++i)
        EXPECT_LE(after.equity_curve[i - 1].timestamp,
                  after.equity_curve[i].timestamp);
}

TEST(Analytics, ExactAndConflictingSameTimestampMarksAreDistinguished)
{
    Analytics analytics(1'000.0);
    const auto mark = [&](double price) {
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(100), "A", price, price, price, price));
    };

    mark(100.0);
    mark(100.0);
    auto report = analytics.generate_report();
    EXPECT_TRUE(report.portfolio_time_series_valid);
    EXPECT_EQ(report.duplicate_market_marks_ignored, 1u);
    EXPECT_EQ(report.conflicting_market_marks_rejected, 0u);

    mark(101.0);
    report = analytics.generate_report();
    EXPECT_FALSE(report.portfolio_time_series_valid);
    EXPECT_EQ(report.portfolio_time_series_reason,
              "conflicting_same_timestamp_market_mark");
    EXPECT_EQ(report.conflicting_market_marks_rejected, 1u);
}

TEST(Analytics, C15_ReportCurveRemainsCausalAfterFundingAndUnrelatedMark)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(0), "A", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(60'000), "A", 0.0, 1.0));
    const auto before = analytics.generate_report();
    // An exact old mark remains an idempotent no-op even though Funding has
    // advanced the global economic clock beyond the mark timestamp.
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(0), "A", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(30'000), "UNRELATED", 10.0, 10.0, 10.0, 10.0));
    const auto after = analytics.generate_report();

    ASSERT_TRUE(before.portfolio_time_series_valid);
    EXPECT_TRUE(after.portfolio_time_series_valid);
    EXPECT_EQ(after.portfolio_time_series_reason,
              before.portfolio_time_series_reason);
    EXPECT_EQ(after.rolling_return_count, before.rolling_return_count);
    EXPECT_EQ(after.sharpe_ratio_valid, before.sharpe_ratio_valid);
    EXPECT_DOUBLE_EQ(after.sharpe_ratio, before.sharpe_ratio);
    EXPECT_EQ(after.late_market_events_rejected,
              before.late_market_events_rejected + 1U);
    EXPECT_EQ(after.duplicate_market_marks_ignored,
              before.duplicate_market_marks_ignored + 1U);
    ASSERT_EQ(after.equity_curve.size(), before.equity_curve.size());
    for (std::size_t i = 1; i < after.equity_curve.size(); ++i)
        EXPECT_LE(after.equity_curve[i - 1].timestamp,
                  after.equity_curve[i].timestamp);
}

TEST(Analytics, C15_ClosedEconomicHistoryCannotBeRelabeledToMarketOnlySymbol)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(1), "A", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(2), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(3), "A", 2, order_side::sell,
        1.0, 110.0, 0.0, 0.0, 2));

    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(4), "B", 10.0, 10.0, 10.0, 10.0));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(5), "B", 20.0, 20.0, 20.0, 20.0));

    const auto report = analytics.generate_report();
    ASSERT_TRUE(report.benchmark_valid);
    EXPECT_EQ(report.benchmark_symbol, "A");
    EXPECT_EQ(report.benchmark_reason, "single_economic_symbol");
    EXPECT_DOUBLE_EQ(report.buy_and_hold_return, 0.0);
    EXPECT_DOUBLE_EQ(report.cumulative_return, 0.01);
}

TEST(Analytics, C15_RejectedFillCannotCreateEconomicSymbolHistory)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(1), "A", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(2), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));

    auto invalid = std::make_shared<fill_event>(
        epoch_ms(3), "B", 2, order_side::buy,
        std::numeric_limits<double>::quiet_NaN(), 10.0, 0.0, 0.0, 2);
    deliver_valid_event(analytics, invalid);
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(4), "B", 10.0, 10.0, 10.0, 10.0));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(5), "B", 20.0, 20.0, 20.0, 20.0));

    const auto report = analytics.generate_report();
    EXPECT_EQ(report.invalid_fill_payloads_rejected, 1u);
    EXPECT_TRUE(report.benchmark_valid);
    EXPECT_EQ(report.benchmark_symbol, "A");
    EXPECT_EQ(report.benchmark_reason, "single_economic_symbol");
}

TEST(Analytics, C15_ResetClearsEconomicSymbolHistory)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    analytics.reset(1'000.0);

    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(2), "B", 10.0, 10.0, 10.0, 10.0));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(60'002), "B", 20.0, 20.0, 20.0, 20.0));
    const auto report = analytics.generate_report();
    ASSERT_TRUE(report.benchmark_valid);
    EXPECT_EQ(report.benchmark_symbol, "B");
    EXPECT_EQ(report.benchmark_reason, "single_market_symbol");
}

TEST(Analytics, C15_FillBeforeFirstMarkBindsEconomicBenchmark)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(2), "B", 10.0, 10.0, 10.0, 10.0));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(3), "A", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(6), "A", 0.0, 1.0));
    const auto before = analytics.generate_report();

    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(4), "B", 10.0, 10.0, 10.0, 10.0));
    const auto after = analytics.generate_report();

    ASSERT_TRUE(before.portfolio_time_series_valid);
    EXPECT_TRUE(after.portfolio_time_series_valid);
    EXPECT_EQ(after.late_market_events_rejected,
              before.late_market_events_rejected + 1U);
    EXPECT_EQ(after.benchmark_symbol, "A");
    EXPECT_EQ(after.benchmark_reason, "single_economic_symbol");
}

TEST(Analytics, TimeInMarketUsesDurationNotMarketEventDensity)
{
    const auto run = [](bool add_dense_marks) {
        Analytics analytics(1'000.0);
        const auto mark = [&](std::int64_t ts_ms, double price) {
            deliver_valid_event(analytics, std::make_shared<market_event>(
                epoch_ms(ts_ms), "A", price, price, price, price));
        };
        mark(0, 100.0);
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(20), "A", 1, order_side::buy,
            1.0, 100.0, 0.0, 0.0, 1));
        if (add_dense_marks)
        {
            mark(25, 100.0);
            mark(30, 100.0);
            mark(40, 100.0);
        }
        mark(50, 100.0);
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(60), "A", 2, order_side::sell,
            1.0, 100.0, 0.0, 0.0, 2));
        mark(100, 100.0);
        return analytics.generate_report();
    };

    const auto sparse = run(false);
    const auto dense = run(true);
    EXPECT_TRUE(sparse.time_in_market_valid);
    EXPECT_EQ(sparse.time_in_market_reason,
              "computed_from_economic_time");
    EXPECT_DOUBLE_EQ(sparse.time_in_market_pct, 40.0);
    EXPECT_DOUBLE_EQ(dense.time_in_market_pct,
                     sparse.time_in_market_pct);
}

TEST(Analytics, TimeInMarketFailsClosedOnNonMonotonicEconomicTime)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(0), "A", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(20), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(100), "A", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(90), "A", 2, order_side::sell,
        1.0, 100.0, 0.0, 0.0, 2));

    const auto report = analytics.generate_report();
    EXPECT_FALSE(report.time_in_market_valid);
    EXPECT_DOUBLE_EQ(report.time_in_market_pct, 0.0);
    EXPECT_EQ(report.time_in_market_reason,
              "non_monotonic_economic_time");
    ASSERT_EQ(report.open_positions.size(), 1u);
    EXPECT_DOUBLE_EQ(report.open_positions.front().quantity, 1.0);
    EXPECT_EQ(report.total_fills, 1u);
}

TEST(Analytics, LateFundingIsTransactionalAndCannotReverseCurveTime)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(0), "A", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(100), "A", 0.0, 1.0));
    const auto before = analytics.generate_report();
    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(50), "A", 0.0, 100.0));
    const auto after = analytics.generate_report();

    EXPECT_DOUBLE_EQ(after.final_equity, before.final_equity);
    EXPECT_DOUBLE_EQ(after.funding_pnl, before.funding_pnl);
    ASSERT_EQ(after.equity_curve.size(), before.equity_curve.size());
    EXPECT_FALSE(after.time_in_market_valid);
    EXPECT_EQ(after.time_in_market_reason,
              "non_monotonic_economic_time");
    EXPECT_EQ(after.late_funding_events_rejected, 1u);
    EXPECT_FALSE(after.accounting_reconciled);
    EXPECT_EQ(after.accounting_reconciliation_reason,
              "unreconciled_funding_settlement");
}

TEST(Analytics, LateFillDoesNotPoisonItsStableIdentity)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(10), "X", 100.0, 100.0, 100.0, 100.0));

    auto late = std::make_shared<fill_event>(
        epoch_ms(9), "X", 7, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 77);
    deliver_valid_event(analytics, late);
    auto corrected = std::make_shared<fill_event>(
        epoch_ms(11), "X", 7, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 77);
    deliver_valid_event(analytics, corrected);

    const auto report = analytics.generate_report();
    EXPECT_EQ(report.total_fills, 1u);
    EXPECT_EQ(report.late_fill_events_rejected, 1u);
    EXPECT_EQ(report.conflicting_fill_replays_rejected, 0u);
    EXPECT_FALSE(report.accounting_reconciled);
    EXPECT_EQ(report.accounting_reconciliation_reason,
              "unreconciled_fill_event");
    EXPECT_FALSE(report.valuation_complete);
    ASSERT_EQ(report.open_positions.size(), 1u);
    EXPECT_DOUBLE_EQ(report.open_positions.front().quantity, 1.0);
}

TEST(Analytics, ExactOldFillReplayIsANoopBeforeEconomicTimeValidation)
{
    Analytics analytics(1'000.0);
    auto fill = std::make_shared<fill_event>(
        epoch_ms(1), "PERP", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 77);
    deliver_valid_event(analytics, fill);
    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(2), "PERP", 0.0, 1.0));
    const auto before = analytics.generate_report();

    deliver_valid_event(analytics, fill);
    const auto after = analytics.generate_report();

    EXPECT_EQ(after.total_fills, before.total_fills);
    EXPECT_DOUBLE_EQ(after.final_equity, before.final_equity);
    EXPECT_DOUBLE_EQ(after.funding_pnl, before.funding_pnl);
    ASSERT_EQ(after.equity_curve.size(), before.equity_curve.size());
    for (std::size_t i = 0; i < before.equity_curve.size(); ++i)
    {
        EXPECT_EQ(after.equity_curve[i].timestamp,
                  before.equity_curve[i].timestamp);
        EXPECT_DOUBLE_EQ(after.equity_curve[i].equity,
                         before.equity_curve[i].equity);
    }
    EXPECT_EQ(after.late_fill_events_rejected,
              before.late_fill_events_rejected);
    EXPECT_EQ(after.duplicate_fill_replays_ignored,
              before.duplicate_fill_replays_ignored + 1U);
    EXPECT_TRUE(after.time_in_market_valid);
}

TEST(Analytics, ConflictingOldFillReplayRejectsWithoutRelabelingAsLate)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "PERP", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 77));
    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(2), "PERP", 0.0, 1.0));
    const auto before = analytics.generate_report();

    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "PERP", 1, order_side::buy,
        2.0, 100.0, 0.0, 0.0, 77));
    const auto after = analytics.generate_report();

    EXPECT_EQ(after.total_fills, before.total_fills);
    EXPECT_DOUBLE_EQ(after.final_equity, before.final_equity);
    EXPECT_EQ(after.late_fill_events_rejected,
              before.late_fill_events_rejected);
    EXPECT_EQ(after.conflicting_fill_replays_rejected,
              before.conflicting_fill_replays_rejected + 1U);
    EXPECT_TRUE(after.time_in_market_valid);
    EXPECT_FALSE(after.accounting_reconciled);
    EXPECT_EQ(after.accounting_reconciliation_reason,
              "unreconciled_fill_event");
}

TEST(Analytics, GloballyLateFirstSymbolCannotReplaceBenchmark)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(0), "A", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(60), "A", 120.0, 120.0, 120.0, 120.0));
    const auto before = analytics.generate_report();
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(0), "0B", 10.0, 10.0, 10.0, 10.0));
    const auto after = analytics.generate_report();

    EXPECT_DOUBLE_EQ(before.buy_and_hold_return, 0.20);
    EXPECT_DOUBLE_EQ(after.buy_and_hold_return,
                     before.buy_and_hold_return);
    ASSERT_EQ(after.benchmark_equity_curve.size(),
              before.benchmark_equity_curve.size());
    for (std::size_t i = 0; i < before.benchmark_equity_curve.size(); ++i)
    {
        EXPECT_EQ(after.benchmark_equity_curve[i].timestamp,
                  before.benchmark_equity_curve[i].timestamp);
        EXPECT_DOUBLE_EQ(after.benchmark_equity_curve[i].equity,
                         before.benchmark_equity_curve[i].equity);
    }
}

TEST(Analytics, ExactMarketReplayDoesNotDecayVolatility)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(0), "A", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(60), "A", 200.0, 200.0, 200.0, 200.0));
    const double before = analytics.risk_view().realized_vol_1h;
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(60), "A", 200.0, 200.0, 200.0, 200.0));
    const double after = analytics.risk_view().realized_vol_1h;

    ASSERT_GT(before, 0.0);
    EXPECT_DOUBLE_EQ(after, before);
}

TEST(Analytics, SameTimestampFillAndMarkHaveOrderInvariantFinalEquity)
{
    const auto run = [](bool fill_first) {
        Analytics analytics(1'000.0);
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(0), "A", 100.0, 100.0, 100.0, 100.0));
        const auto fill = std::make_shared<fill_event>(
            epoch_ms(100), "A", 1, order_side::buy,
            1.0, 100.0, 0.0, 0.0, 1);
        const auto mark = std::make_shared<market_event>(
            epoch_ms(100), "A", 110.0, 110.0, 110.0, 110.0);
        if (fill_first)
        {
            deliver_valid_event(analytics, fill);
            deliver_valid_event(analytics, mark);
        }
        else
        {
            deliver_valid_event(analytics, mark);
            deliver_valid_event(analytics, fill);
        }
        return analytics.generate_report();
    };

    const auto fill_first = run(true);
    const auto mark_first = run(false);
    EXPECT_DOUBLE_EQ(fill_first.final_equity, 1'010.0);
    EXPECT_DOUBLE_EQ(mark_first.final_equity,
                     fill_first.final_equity);
    ASSERT_EQ(fill_first.open_positions.size(), 1u);
    ASSERT_EQ(mark_first.open_positions.size(), 1u);
    EXPECT_DOUBLE_EQ(fill_first.open_positions.front().mark, 110.0);
    EXPECT_DOUBLE_EQ(mark_first.open_positions.front().mark, 110.0);
}

TEST(Analytics, ExtremeClockRangeDoesNotOverflowTimeAccounting)
{
    Analytics analytics(1'000.0);
    const auto first = std::chrono::system_clock::time_point::min();
    const auto last = std::chrono::system_clock::time_point::max();
    deliver_valid_event(analytics, std::make_shared<market_event>(
        first, "A", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        first, "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        last, "A", 101.0, 101.0, 101.0, 101.0));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        last, "A", 2, order_side::sell,
        1.0, 101.0, 0.0, 0.0, 2));

    const auto report = analytics.generate_report();
    EXPECT_TRUE(report.time_in_market_valid);
    EXPECT_TRUE(report.annualized_return_valid);
    EXPECT_TRUE(std::isfinite(report.time_in_market_pct));
    EXPECT_TRUE(std::isfinite(report.annualized_return));
    EXPECT_TRUE(std::isfinite(report.avg_holding_period_ms));
}

TEST(Analytics, AnnualizedReturnUsesCausalElapsedTime)
{
    const auto run = [](std::chrono::days horizon) {
        Analytics analytics(/*initial_cash=*/1'000.0,
                            /*rolling_window=*/16,
                            /*risk_free_rate=*/0.0,
                            /*periods_per_year=*/365);
        deliver_valid_event(analytics, std::make_shared<market_event>(
            epoch_ms(0), "A", 100.0, 100.0, 100.0, 100.0));
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(0), "A", 1, order_side::buy,
            1.0, 100.0, 0.0, 0.0, 1));
        const auto end = std::chrono::system_clock::time_point{horizon};
        deliver_valid_event(analytics, std::make_shared<market_event>(
            end, "A", 101.0, 101.0, 101.0, 101.0));
        return analytics.generate_report();
    };

    const auto one_year = run(std::chrono::days{365});
    const auto thirty_days = run(std::chrono::days{30});
    ASSERT_NEAR(one_year.cumulative_return, 0.001, 1e-12);
    EXPECT_NEAR(one_year.annualized_return, 0.001, 1e-12);
    EXPECT_NEAR(thirty_days.annualized_return,
                std::pow(1.001, 365.0 / 30.0) - 1.0,
                1e-12);
    EXPECT_GT(thirty_days.annualized_return,
              one_year.annualized_return);
}

TEST(Analytics, CalmarRequiresValidAnnualizationAndPositiveDrawdown)
{
    Analytics analytics(/*initial_cash=*/1'000.0,
                        /*rolling_window=*/16,
                        /*risk_free_rate=*/0.0,
                        /*periods_per_year=*/365);
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(0), "A", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(0), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        std::chrono::system_clock::time_point{std::chrono::days{182}},
        "A", 90.0, 90.0, 90.0, 90.0));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        std::chrono::system_clock::time_point{std::chrono::days{365}},
        "A", 110.0, 110.0, 110.0, 110.0));

    const auto report = analytics.generate_report();
    ASSERT_TRUE(report.annualized_return_valid);
    ASSERT_TRUE(report.calmar_ratio_valid);
    EXPECT_EQ(report.calmar_ratio_reason,
              "computed_from_annualized_return_and_drawdown");
    EXPECT_NEAR(report.annualized_return, 0.01, 1e-12);
    EXPECT_NEAR(report.max_drawdown, 1.0, 1e-12);
    EXPECT_NEAR(report.calmar_ratio, 1.0, 1e-12);

    Analytics no_horizon(1'000.0);
    const auto unsupported = no_horizon.generate_report();
    EXPECT_FALSE(unsupported.calmar_ratio_valid);
    EXPECT_EQ(unsupported.calmar_ratio_reason,
              "annualized_return_unavailable");
}

TEST(Analytics, RiskView_EquityPopulatedFromMarketEvents)
{
    Analytics a(100000.0);
    EXPECT_DOUBLE_EQ(a.risk_view().equity, 0.0); // nothing seen yet

    auto m = std::make_shared<market_event>(epoch_ms(0), "X", 100, 100, 100, 100.0);
    deliver_valid_event(a, m);
    EXPECT_NEAR(a.risk_view().equity, 100000.0, 1e-6);

    auto bf = std::make_shared<fill_event>(epoch_ms(1), "X", 1, order_side::buy, 10, 100.0, 0.0);
    deliver_valid_event(a, bf);
    auto m2 = std::make_shared<market_event>(epoch_ms(2), "X", 110, 110, 110, 110.0);
    deliver_valid_event(a, m2);
    EXPECT_NEAR(a.risk_view().equity, 100100.0, 1e-6);
}

TEST(Analytics, RiskViewUpdatesEquityAndDrawdownImmediatelyAfterFill)
{
    Analytics a(1000.0);
    deliver_valid_event(a, std::make_shared<market_event>(
        epoch_ms(0), "X", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(a, std::make_shared<fill_event>(
        epoch_ms(1), "X", 1, order_side::buy, 10.0, 100.0, 0.0));

    // No later market event: the close itself must make the loss visible to
    // the post-fill RiskManager in the same engine event.
    deliver_valid_event(a, std::make_shared<fill_event>(
        epoch_ms(2), "X", 2, order_side::sell, 10.0, 80.0, 0.0));

    const auto rv = a.risk_view();
    EXPECT_DOUBLE_EQ(rv.equity, 800.0);
    EXPECT_DOUBLE_EQ(rv.max_drawdown, 20.0);
}

TEST(Analytics, TickAndFundingUpdateDrawdownImmediately)
{
    Analytics a(1000.0);
    deliver_valid_event(a, std::make_shared<market_event>(
        epoch_ms(0), "X", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(a, std::make_shared<fill_event>(
        epoch_ms(1), "X", 1, order_side::buy, 10.0, 100.0, 0.0));
    deliver_valid_event(a, std::make_shared<tick_event>(
        epoch_ms(2), "X", 150.0, 1, tick_side::bid));
    deliver_valid_event(a, std::make_shared<tick_event>(
        epoch_ms(3), "X", 80.0, 1, tick_side::bid));

    auto rv = a.risk_view();
    EXPECT_NEAR(rv.equity, 800.0, 1e-9);
    EXPECT_NEAR(rv.max_drawdown, 700.0 / 1500.0 * 100.0, 1e-9);

    deliver_valid_event(a, std::make_shared<funding_event>(
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
    deliver_valid_event(a, mkt0);
    auto bf = std::make_shared<fill_event>(epoch_ms(0), "X", 1, order_side::buy, 100, 100.0, 0.0);
    deliver_valid_event(a, bf);
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

    const auto period = std::chrono::duration_cast<
        std::chrono::system_clock::duration>(std::chrono::days{365})
        / static_cast<std::int64_t>(ppy);
    auto t = epoch_ms(0) + period;
    for (double p : prices)
    {
        auto m = std::make_shared<market_event>(t, "X", p, p, p, p);
        deliver_valid_event(a, m);
        t += period;
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
    EXPECT_TRUE(report.sortino_ratio_valid);
    EXPECT_EQ(report.sortino_ratio_reason,
              "computed_at_configured_cadence");
    EXPECT_NEAR(report.sortino_ratio, expected_sortino, 1e-9);
}

TEST(Analytics, FundingEvent_UpdatesCashAndEquityAndRiskView)
{
    Analytics a(100000.0);

    deliver_valid_event(a, std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));

    // Simulate a funding credit from the exchange
    auto funding_ev = std::make_shared<funding_event>(
        epoch_ms(1000), "BTCUSDT", 0.0, 250.0, "FUNDING_FEE");

    deliver_valid_event(a, funding_ev);

    auto rv = a.risk_view();
    EXPECT_NEAR(rv.equity, 100250.0, 1e-6);
    EXPECT_NEAR(a.total_funding_pnl(), 250.0, 1e-6);

    auto report = a.generate_report();
    // Equity curve should have recorded the funding adjustment point
    EXPECT_GE(report.equity_curve.size(), 1u);
}

TEST(Analytics, ReconciliationSeparatesGrossFeesFundingAndUnrealized)
{
    Analytics a(1000.0);
    deliver_valid_event(a, std::make_shared<market_event>(
        epoch_ms(0), "BTCUSDT", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(a, std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 1, order_side::buy, 1.0, 100.0,
        1.0, 0.0, 1));
    deliver_valid_event(a, std::make_shared<funding_event>(
        epoch_ms(2), "BTCUSDT", 0.0, 5.0, "FUNDING_FEE"));
    deliver_valid_event(a, std::make_shared<fill_event>(
        epoch_ms(3), "BTCUSDT", 2, order_side::sell, 1.0, 110.0,
        2.0, 0.0, 2));

    const auto r = a.generate_report();
    EXPECT_NEAR(r.gross_realized_pnl, 10.0, 1e-12);
    EXPECT_NEAR(r.realized_pnl, 7.0, 1e-12);
    EXPECT_NEAR(r.total_commission, 3.0, 1e-12);
    EXPECT_NEAR(r.funding_pnl, 5.0, 1e-12);
    EXPECT_NEAR(r.unrealized_pnl, 0.0, 1e-12);
    EXPECT_NEAR(r.final_equity, 1012.0, 1e-12);
    EXPECT_NEAR(r.reconciliation_residual, 0.0, 1e-12);
}

TEST(Analytics, JsonExportIncludesAccountingAndDrawdownContract)
{
    Analytics a(1000.0);
    deliver_valid_event(a, std::make_shared<market_event>(
        epoch_ms(0), "BTCUSDT", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(a, std::make_shared<fill_event>(
        epoch_ms(0), "BTCUSDT", 1, order_side::buy, 1.0, 100.0,
        1.0, 0.0, 1));
    deliver_valid_event(a, std::make_shared<fill_event>(
        std::chrono::system_clock::time_point{std::chrono::days{365}},
        "BTCUSDT", 2, order_side::sell, 1.0, 110.0,
        2.0, 0.0, 2));

    const auto path = std::filesystem::temp_directory_path()
        / "truetest_analytics_accounting_export.json";
    const std::string canonical = a.generate_report().to_results_json() + "\n";
    a.export_json(path.string());
    std::ifstream in(path);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), {});
    std::filesystem::remove(path);

    EXPECT_EQ(json, canonical);

    const auto parsed_contract = nlohmann::json::parse(json);
    EXPECT_DOUBLE_EQ(parsed_contract.at("gross_realized_pnl").get<double>(), 10.0);
    EXPECT_DOUBLE_EQ(parsed_contract.at("realized_pnl").get<double>(), 7.0);
    EXPECT_DOUBLE_EQ(parsed_contract.at("total_commission").get<double>(), 3.0);
    EXPECT_NE(json.find("\"periods_per_year\":525600"), std::string::npos);
    EXPECT_NE(json.find("\"return_observation_basis\":\"market_marks_excluding_cash_settlements\""),
              std::string::npos);
    EXPECT_NE(json.find("\"equity_curve_sample_stride\""), std::string::npos);
    EXPECT_NE(json.find("\"benchmark_equity_curve_sample_stride\""),
              std::string::npos);
    EXPECT_NE(json.find("\"benchmark_curve_observation_basis\":\"selected_symbol_market_marks\""),
              std::string::npos);
    EXPECT_NE(json.find("\"max_drawdown_source\":\"full_resolution_mark_stream\""),
              std::string::npos);
    EXPECT_NE(json.find("\"rolling_sharpe_reason\":\"insufficient_return_observations\""),
              std::string::npos);
    EXPECT_NE(json.find("\"time_in_market_valid\":true"),
              std::string::npos);
    EXPECT_NE(json.find("\"time_in_market_reason\":\"computed_from_economic_time\""),
              std::string::npos);
    EXPECT_NE(json.find("\"invalid_fill_payloads_rejected\""),
              std::string::npos);
    EXPECT_NE(json.find("\"unreconciled_funding_events_rejected\""),
              std::string::npos);
    EXPECT_NE(json.find("\"duplicate_funding_replays_ignored\""),
              std::string::npos);
    EXPECT_NE(json.find("\"conflicting_funding_replays_rejected\""),
              std::string::npos);
    EXPECT_NE(json.find("\"late_fill_events_rejected\""),
              std::string::npos);
    EXPECT_NE(json.find("\"late_funding_events_rejected\""),
              std::string::npos);
    EXPECT_NE(json.find("\"late_market_events_rejected\""),
              std::string::npos);
    EXPECT_NE(json.find("\"duplicate_market_marks_ignored\""),
              std::string::npos);
    EXPECT_NE(json.find("\"conflicting_market_marks_rejected\""),
              std::string::npos);
    EXPECT_NE(json.find("\"portfolio_time_series_valid\""),
              std::string::npos);
    EXPECT_NE(json.find("\"portfolio_time_series_reason\""),
              std::string::npos);
    EXPECT_NE(json.find(
                  "\"ambiguous_portfolio_mark_sequences_rejected\""),
              std::string::npos);
    EXPECT_TRUE(parsed_contract.is_object());
    EXPECT_NE(json.find("\"annualized_return_valid\":true"),
              std::string::npos);
    EXPECT_NE(json.find(
                  "\"annualized_return_reason\":\"computed_from_causal_elapsed_time\""),
              std::string::npos);
    EXPECT_NE(json.find(
                  "\"annualized_return_basis\":\"causal_elapsed_time_365d\""),
              std::string::npos);
    EXPECT_NE(json.find("\"intended_price\""), std::string::npos);
    EXPECT_NE(json.find("\"trade_rows_kind\":\"physical_fill_legs\""),
              std::string::npos);
}

TEST(Analytics, JsonExportDoesNotTruncateLargeFiniteValues)
{
    Analytics analytics(std::numeric_limits<double>::max() / 4.0);
    const auto path = std::filesystem::temp_directory_path()
        / "truetest_analytics_large_export.json";
    analytics.export_json(path.string());
    std::ifstream in(path);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), {});
    std::filesystem::remove(path);

    EXPECT_NO_THROW({
        const auto parsed = nlohmann::json::parse(json);
        EXPECT_TRUE(parsed.is_object());
    });
}

TEST(Analytics, JsonExportEscapesDynamicTradeText)
{
    Analytics analytics(1'000.0);
    const std::string strategy = "bad\"\\strategy\nname";
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1, strategy, 1));
    const auto path = std::filesystem::temp_directory_path()
        / "truetest_analytics_escaped_export.json";
    analytics.export_json(path.string());
    std::ifstream in(path);
    ASSERT_TRUE(in.is_open());
    const std::string json((std::istreambuf_iterator<char>(in)), {});
    std::filesystem::remove(path);

    const auto parsed = nlohmann::json::parse(json);
    ASSERT_EQ(parsed.at("trades").size(), 1u);
    EXPECT_EQ(parsed.at("trades").front().at("strategy_name"), strategy);
}

TEST(Analytics, FillCarriesStrategyWhenRecordedOrderIsUnavailable)
{
    Analytics a(10'000.0);
    auto fill = std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 77, order_side::buy, 1.0, 100.0,
        0.0, 0.0, 1, "ledger_strategy", 77);

    deliver_valid_event(a, fill);
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
    deliver_valid_event(a, fill_a_open);

    // Strategy B opens 3 BTC Short @ 51,000 (Order 201)
    auto fill_b_open = std::make_shared<fill_event>(
        epoch_ms(2), "BTCUSDT", 201, order_side::sell, 3.0, 51000.0,
        0.0, 0.0, 1, "strat_b", 201);
    deliver_valid_event(a, fill_b_open);

    // At this point, no trades have closed yet; both positions are open and isolated.
    auto r_mid = a.generate_report();
    EXPECT_EQ(r_mid.total_trades, 0u);
    EXPECT_DOUBLE_EQ(r_mid.realized_pnl, 0.0);

    // Strategy A closes 10 BTC Long @ 55,000 (Order 102) -> PnL: +50,000
    auto fill_a_close = std::make_shared<fill_event>(
        epoch_ms(3), "BTCUSDT", 102, order_side::sell, 10.0, 55000.0,
        0.0, 0.0, 1, "strat_a", 101);
    deliver_valid_event(a, fill_a_close);

    // Strategy B closes 3 BTC Short @ 49,000 (Order 202) -> PnL: (51000 - 49000) * 3 = +6,000
    auto fill_b_close = std::make_shared<fill_event>(
        epoch_ms(4), "BTCUSDT", 202, order_side::buy, 3.0, 49000.0,
        0.0, 0.0, 1, "strat_b", 201);
    deliver_valid_event(a, fill_b_close);

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

TEST(Analytics, DuplicatePhysicalFillIsIgnoredBeforeLedgerMutation)
{
    Analytics a(100000.0);

    // Initial position: 10 BTC Long @ 100.0
    auto ord1 = std::make_shared<order_event>(epoch_ms(1), "BTCUSDT", order_type::limit, order_side::buy, 10.0, 100.0);
    ord1->set_order_id(1);
    deliver_valid_event(a, ord1);
    auto fill1 = std::make_shared<fill_event>(epoch_ms(1), "BTCUSDT", 1, order_side::buy, 10.0, 100.0, 0.0, 0.0, 1, "strat", 1);
    deliver_valid_event(a, fill1);

    // Flip order: Sell 15 BTC @ intended 100.0, but filled @ 98.0 (2.0 slippage)
    auto ord2 = std::make_shared<order_event>(epoch_ms(2), "BTCUSDT", order_type::market, order_side::sell, 15.0, 100.0);
    ord2->set_order_id(2);
    deliver_valid_event(a, ord2);

    auto physical_flip = std::make_shared<fill_event>(
        epoch_ms(2), "BTCUSDT", 2, order_side::sell, 15.0, 98.0, 0.0, 0.0, 42, "strat", 1);

    deliver_valid_event(a, physical_flip);
    deliver_valid_event(a, physical_flip); // duplicate delivery must be a complete no-op

    auto r = a.generate_report();
    // 1 opener fill + 1 physical flip fill = 2 fills total (not 3)
    EXPECT_EQ(r.total_fills, 2u);
    ASSERT_EQ(r.trades.size(), 2u);
    ASSERT_EQ(r.open_positions.size(), 1u);
    EXPECT_NEAR(r.open_positions.front().quantity, -5.0, 1e-12);
    // Slippage should be evaluated once for order 2 (2.0 slippage, avg = 1.0 across orders 1 and 2 or 2.0 if order 1 had 0 slippage)
    // Order 1 had intended 100, filled 100 -> slip = 0. Order 2 had intended 100, filled 98 -> slip = 2.
    // Total slippage = 2.0, slippage_count = 2, avg_slippage = 1.0.
    EXPECT_DOUBLE_EQ(r.avg_slippage, 1.0);
    EXPECT_EQ(r.duplicate_fill_replays_ignored, 1u);
    EXPECT_EQ(r.conflicting_fill_replays_rejected, 0u);
}

TEST(Analytics, MissingAuthoritativeFillIdentityFailsClosed)
{
    Analytics analytics(100'000.0);
    auto anonymous = std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 7, order_side::buy,
        1.0, 100.0, 0.0);

    // Deliberately bypass the fixture helper: this is malformed canonical
    // ingress and cannot be made idempotent.
    analytics.on_event(anonymous);
    analytics.on_event(anonymous);

    const auto report = analytics.generate_report();
    EXPECT_EQ(report.total_fills, 0u);
    EXPECT_TRUE(report.trades.empty());
    EXPECT_TRUE(report.open_positions.empty());
    EXPECT_EQ(report.missing_fill_identities_rejected, 2u);
    EXPECT_FALSE(report.accounting_reconciled);
    EXPECT_EQ(report.accounting_reconciliation_reason,
              "unreconciled_fill_event");
    EXPECT_FALSE(report.valuation_complete);
}

TEST(Analytics, OverflowingFillNotionalFailsClosed)
{
    Analytics analytics(100'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 7, order_side::buy,
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max(), 0.0));

    const auto report = analytics.generate_report();
    EXPECT_EQ(report.total_fills, 0u);
    EXPECT_TRUE(report.trades.empty());
    EXPECT_TRUE(report.open_positions.empty());
    EXPECT_TRUE(std::isfinite(report.final_equity));
    EXPECT_EQ(report.invalid_fill_payloads_rejected, 1u);
    EXPECT_FALSE(report.accounting_reconciled);
    EXPECT_EQ(report.accounting_reconciliation_reason,
              "unreconciled_fill_event");
}

TEST(Analytics, AggregatePositionQuantityOverflowFailsClosed)
{
    Analytics analytics(1'000.0);
    const double quantity = std::numeric_limits<double>::max() * 0.6;
    const double price = std::numeric_limits<double>::min();
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "TINY", 1, order_side::buy,
        quantity, price, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(2), "TINY", 2, order_side::buy,
        quantity, price, 0.0, 0.0, 2));

    const auto report = analytics.generate_report();
    EXPECT_EQ(report.total_fills, 1u);
    EXPECT_EQ(report.invalid_fill_payloads_rejected, 1u);
    ASSERT_EQ(report.open_positions.size(), 1u);
    EXPECT_TRUE(std::isfinite(report.open_positions.front().quantity));
    EXPECT_TRUE(std::isfinite(report.final_equity));
}

TEST(Analytics, ExactFundingReplayIsACompleteNoOp)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "PERP", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    auto settlement = std::make_shared<funding_event>(
        epoch_ms(2), "PERP", 0.0, -1.0);
    deliver_valid_event(analytics, settlement);
    deliver_valid_event(analytics, settlement);

    const auto report = analytics.generate_report();
    EXPECT_DOUBLE_EQ(report.funding_pnl, -1.0);
    EXPECT_EQ(report.duplicate_funding_replays_ignored, 1u);
    EXPECT_EQ(report.conflicting_funding_replays_rejected, 0u);
}

TEST(Analytics, ExactOldFundingReplayIsANoopBeforeEconomicTimeValidation)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "PERP", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    auto settlement = std::make_shared<funding_event>(
        epoch_ms(2), "PERP", 0.0, -1.0);
    deliver_valid_event(analytics, settlement);
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(3), "PERP", 101.0, 101.0, 101.0, 101.0));
    const auto before = analytics.generate_report();

    deliver_valid_event(analytics, settlement);
    const auto after = analytics.generate_report();

    EXPECT_DOUBLE_EQ(after.final_equity, before.final_equity);
    EXPECT_DOUBLE_EQ(after.funding_pnl, before.funding_pnl);
    ASSERT_EQ(after.equity_curve.size(), before.equity_curve.size());
    for (std::size_t i = 0; i < before.equity_curve.size(); ++i)
    {
        EXPECT_EQ(after.equity_curve[i].timestamp,
                  before.equity_curve[i].timestamp);
        EXPECT_DOUBLE_EQ(after.equity_curve[i].equity,
                         before.equity_curve[i].equity);
    }
    EXPECT_EQ(after.late_funding_events_rejected,
              before.late_funding_events_rejected);
    EXPECT_EQ(after.duplicate_funding_replays_ignored,
              before.duplicate_funding_replays_ignored + 1U);
    EXPECT_TRUE(after.time_in_market_valid);
}

TEST(Analytics, ConflictingOldFundingReplayLatchesBeforeLateClassification)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "PERP", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(2), "PERP", 0.0, -1.0));
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(3), "PERP", 101.0, 101.0, 101.0, 101.0));
    const auto before = analytics.generate_report();

    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(2), "PERP", 0.0, -2.0));
    const auto after = analytics.generate_report();

    EXPECT_DOUBLE_EQ(after.funding_pnl, before.funding_pnl);
    EXPECT_EQ(after.late_funding_events_rejected,
              before.late_funding_events_rejected);
    EXPECT_EQ(after.conflicting_funding_replays_rejected,
              before.conflicting_funding_replays_rejected + 1U);
    EXPECT_FALSE(after.accounting_reconciled);
    EXPECT_EQ(after.accounting_reconciliation_reason,
              "unreconciled_funding_settlement");
    EXPECT_FALSE(after.valuation_complete);
    EXPECT_EQ(analytics.risk_view().current_funding_8h_rate,
              std::numeric_limits<double>::max());
    EXPECT_TRUE(after.time_in_market_valid);
}

TEST(Analytics, ConflictingFundingReplayLatchesRiskUntilReset)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "PERP", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(2), "PERP", 0.0, -1.0));
    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(2), "PERP", 0.0, -2.0));
    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(3), "PERP", 0.0, -1.0));
    analytics.set_current_funding_rate_8h(0.001);

    const auto report = analytics.generate_report();
    EXPECT_DOUBLE_EQ(report.funding_pnl, -2.0);
    EXPECT_EQ(report.conflicting_funding_replays_rejected, 1u);
    EXPECT_FALSE(report.accounting_reconciled);
    EXPECT_EQ(report.accounting_reconciliation_reason,
              "unreconciled_funding_settlement");
    EXPECT_FALSE(report.valuation_complete);
    EXPECT_EQ(analytics.risk_view().current_funding_8h_rate,
              std::numeric_limits<double>::max());
}

TEST(Analytics, FundingPositionAdjustmentIsRejectedTransactionally)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "PERP", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    const auto before = analytics.generate_report();
    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(2), "PERP", 1.0, -1.0));
    const auto after = analytics.generate_report();

    EXPECT_DOUBLE_EQ(after.final_equity, before.final_equity);
    EXPECT_DOUBLE_EQ(after.funding_pnl, before.funding_pnl);
    EXPECT_EQ(after.unreconciled_funding_events_rejected, 1u);
    EXPECT_FALSE(after.accounting_reconciled);
    EXPECT_EQ(after.accounting_reconciliation_reason,
              "unreconciled_funding_settlement");
    EXPECT_FALSE(after.valuation_complete);
    EXPECT_EQ(analytics.risk_view().current_funding_8h_rate,
              std::numeric_limits<double>::max());
}

TEST(Analytics, NetZeroHedgedFundingNotionalFailsClosed)
{
    Analytics analytics(1'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "PERP", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1, "long"));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "PERP", 2, order_side::sell,
        1.0, 100.0, 0.0, 0.0, 2, "short"));
    const auto before = analytics.generate_report();
    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(2), "PERP", 0.0, -2.0));
    const auto after = analytics.generate_report();

    EXPECT_DOUBLE_EQ(after.final_equity, before.final_equity);
    EXPECT_DOUBLE_EQ(after.funding_pnl, 0.0);
    EXPECT_EQ(after.unreconciled_funding_events_rejected, 1u);
    EXPECT_EQ(analytics.risk_view().current_funding_8h_rate,
              std::numeric_limits<double>::max());
}

TEST(Analytics, FundingThatWouldOverflowTotalEquityFailsClosed)
{
    Analytics analytics(std::numeric_limits<double>::max() / 2.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "PERP", 1, order_side::buy,
        1.0, 1.0, 0.0, 0.0, 1));
    const double mark = std::numeric_limits<double>::max() / 2.0;
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(2), "PERP", mark, mark, mark, mark));
    const auto before = analytics.generate_report();
    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(3), "PERP", 0.0,
        std::numeric_limits<double>::max() / 4.0));
    const auto after = analytics.generate_report();

    EXPECT_DOUBLE_EQ(after.final_equity, before.final_equity);
    EXPECT_DOUBLE_EQ(after.funding_pnl, before.funding_pnl);
    EXPECT_EQ(after.unreconciled_funding_events_rejected, 1u);
    EXPECT_TRUE(std::isfinite(after.final_equity));
    EXPECT_EQ(analytics.risk_view().current_funding_8h_rate,
              std::numeric_limits<double>::max());
}

TEST(Analytics, FiniteFundingAndMarkOverflowFailClosed)
{
    Analytics funding(std::numeric_limits<double>::max() / 2.0);
    deliver_valid_event(funding, std::make_shared<fill_event>(
        epoch_ms(1), "PERP", 1, order_side::buy,
        1.0, 1.0, 0.0, 0.0, 1));
    deliver_valid_event(funding, std::make_shared<funding_event>(
        epoch_ms(2), "PERP", 0.0,
        std::numeric_limits<double>::max() / 2.0));
    const auto before = funding.generate_report();
    deliver_valid_event(funding, std::make_shared<funding_event>(
        epoch_ms(3), "PERP", 0.0,
        std::numeric_limits<double>::max() / 2.0));
    const auto after = funding.generate_report();
    EXPECT_DOUBLE_EQ(after.final_equity, before.final_equity);
    EXPECT_DOUBLE_EQ(after.funding_pnl, before.funding_pnl);
    EXPECT_TRUE(std::isfinite(after.final_equity));

    Analytics marked(1'000.0);
    deliver_valid_event(marked, std::make_shared<fill_event>(
        epoch_ms(1), "X", 1, order_side::buy,
        2.0, 1.0, 0.0, 0.0, 1));
    deliver_valid_event(marked, std::make_shared<market_event>(
        epoch_ms(2), "X",
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max()));
    const auto mark_report = marked.generate_report();
    ASSERT_EQ(mark_report.open_positions.size(), 1u);
    EXPECT_DOUBLE_EQ(mark_report.open_positions.front().mark, 1.0);
    EXPECT_FALSE(mark_report.open_positions.front().mark_valid);
    EXPECT_TRUE(std::isfinite(mark_report.final_equity));
}

TEST(Analytics, StableFillIdentityNeverAgesOut)
{
    Analytics analytics(100'000.0);
    for (std::uint64_t id = 1; id <= 9; ++id)
    {
        deliver_valid_event(analytics, std::make_shared<fill_event>(
            epoch_ms(1), "BTCUSDT", id, order_side::buy,
            1.0, 100.0, 0.0, 0.0, id, "strat", id));
    }
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1, "strat", 1));

    const auto report = analytics.generate_report();
    EXPECT_EQ(report.total_fills, 9u);
    ASSERT_EQ(report.open_positions.size(), 1u);
    EXPECT_DOUBLE_EQ(report.open_positions.front().quantity, 9.0);
}

TEST(Analytics, ContradictoryPayloadForStableFillIdentityIsRejected)
{
    Analytics analytics(100'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 7, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 42, "strat", 7));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 7, order_side::buy,
        2.0, 100.0, 0.0, 0.0, 42, "strat", 7));

    const auto report = analytics.generate_report();
    EXPECT_EQ(report.total_fills, 1u);
    EXPECT_EQ(report.conflicting_fill_replays_rejected, 1u);
    ASSERT_EQ(report.open_positions.size(), 1u);
    EXPECT_DOUBLE_EQ(report.open_positions.front().quantity, 1.0);
}

TEST(Analytics, VenueExecutionIdentityDeduplicatesReconnectLocalIds)
{
    Analytics analytics(100'000.0);
    auto original = std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 7, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 41, "strat", 7);
    auto replay = std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 7, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 99, "strat", 7);
    ASSERT_TRUE(original->set_venue_execution_id("venue-trade-123"));
    ASSERT_TRUE(replay->set_venue_execution_id("venue-trade-123"));

    deliver_valid_event(analytics, original);
    deliver_valid_event(analytics, std::make_shared<market_event>(
        epoch_ms(2), "BTCUSDT", 101.0, 101.0, 101.0, 101.0));
    deliver_valid_event(analytics, replay);

    const auto report = analytics.generate_report();
    EXPECT_EQ(report.total_fills, 1u);
    ASSERT_EQ(report.open_positions.size(), 1u);
    EXPECT_DOUBLE_EQ(report.open_positions.front().quantity, 1.0);
    EXPECT_EQ(report.duplicate_fill_replays_ignored, 1u);
    EXPECT_EQ(report.late_fill_events_rejected, 0u);
    EXPECT_TRUE(report.time_in_market_valid);
}

TEST(Analytics, FundingResetsMarketReturnBaselineWithoutSyntheticPeriod)
{
    Analytics a(1000.0);
    deliver_valid_event(a, std::make_shared<market_event>(
        epoch_ms(0), "BTCUSDT", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(a, std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(a, std::make_shared<funding_event>(
        epoch_ms(60'000), "BTCUSDT", 0.0, 10.0, "FUNDING_FEE"));
    // Same timestamp as the funding settlement: it remains one market
    // observation, not a second annualized period.
    deliver_valid_event(a, std::make_shared<market_event>(
        epoch_ms(60'000), "BTCUSDT", 100.0, 100.0, 100.0, 100.0));
    deliver_valid_event(a, std::make_shared<market_event>(
        epoch_ms(120'000), "BTCUSDT", 100.0, 100.0, 100.0, 100.0));

    const auto r = a.generate_report();
    EXPECT_NEAR(r.funding_pnl, 10.0, 1e-12);
    EXPECT_EQ(r.return_observation_basis,
              "market_marks_excluding_cash_settlements");
    // The two market marks after the initial baseline produce two returns;
    // funding resets that baseline instead of adding an irregular sample.
    EXPECT_EQ(r.rolling_return_count, 2u);
    EXPECT_EQ(r.rolling_sharpe_reason, "zero_return_variance");
}

TEST(Analytics, CsvExportLabelsPhysicalFillLegs)
{
    Analytics a(1000.0);
    deliver_valid_event(a, std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 1, order_side::buy, 1.0, 100.0,
        0.0, 0.0, 7));

    const auto base = std::filesystem::temp_directory_path()
        / "truetest_analytics_physical_fill_rows";
    const auto equity_path = base.string() + "-equity.csv";
    const auto trades_path = base.string() + "-trades.csv";
    a.export_csv(equity_path, trades_path);

    std::ifstream in(trades_path);
    ASSERT_TRUE(in.is_open());
    std::string header;
    std::string row;
    ASSERT_TRUE(std::getline(in, header));
    ASSERT_TRUE(std::getline(in, row));
    std::filesystem::remove(equity_path);
    std::filesystem::remove(trades_path);

    EXPECT_EQ(header.find("row_kind,"), 0u);
    EXPECT_EQ(row.find("physical_fill_leg,"), 0u);
}

TEST(Analytics, MultiStrategy_FundingRateDerivation)
{
    Analytics a(100000.0);

    // Strategy A opens 2 BTC @ 50,000 (notional = 100,000)
    auto fill_a = std::make_shared<fill_event>(
        epoch_ms(1), "BTCUSDT", 1, order_side::buy, 2.0, 50000.0, 0.0, 0.0, 1, "strat_a", 1);
    deliver_valid_event(a, fill_a);

    // Strategy B opens 3 BTC @ 50,000 (notional = 150,000)
    auto fill_b = std::make_shared<fill_event>(
        epoch_ms(2), "BTCUSDT", 2, order_side::buy, 3.0, 50000.0, 0.0, 0.0, 2, "strat_b", 2);
    deliver_valid_event(a, fill_b);

    // Total symbol notional = 250,000. Funding cash delta = -25.0 -> rate should be 0.0001 (1 bps)
    funding_event fe(epoch_ms(3), "BTCUSDT", 0.0, -25.0);
    deliver_valid_event(a, std::make_shared<funding_event>(fe));

    auto snap = a.risk_view();
    EXPECT_TRUE(snap.funding_rate_known);
    EXPECT_NEAR(snap.current_funding_8h_rate, 0.0001, 1e-9);
}

TEST(Analytics, CrossSymbolFundingRiskRetainsWorstKnownRate)
{
    Analytics analytics(100'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "A", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(2), "B", 2, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 2));

    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(3), "A", 0.0, -1.0));
    ASSERT_NEAR(analytics.risk_view().current_funding_8h_rate,
                0.01, 1e-12);

    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(4), "B", 0.0, -0.01));
    const auto view = analytics.risk_view();
    EXPECT_TRUE(view.funding_rate_known);
    EXPECT_NEAR(view.current_funding_8h_rate, 0.01, 1e-12);
}

TEST(Analytics, ShortFundingCostProducesPositiveAdverseRiskRate)
{
    Analytics analytics(100'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "PERP", 1, order_side::sell,
        1.0, 100.0, 0.0, 0.0, 1));
    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(2), "PERP", 0.0, -2.0));

    const auto view = analytics.risk_view();
    ASSERT_TRUE(view.funding_rate_known);
    EXPECT_NEAR(view.current_funding_8h_rate, 0.02, 1e-12);

    risk_limits limits;
    limits.max_funding_8h_rate = 0.005;
    RiskManager risk_manager(limits);
    portfolio account(100'000.0);
    order_event candidate(epoch_ms(3), "PERP", order_type::limit,
                          order_side::buy, 1.0, 100.0);
    EXPECT_EQ(risk_manager.check_order(candidate, account, view, 0),
              risk_action::halt);
}

TEST(Analytics, SettlementCannotMaskWorseExternalFundingRate)
{
    Analytics analytics(100'000.0);
    deliver_valid_event(analytics, std::make_shared<fill_event>(
        epoch_ms(1), "PERP", 1, order_side::buy,
        1.0, 100.0, 0.0, 0.0, 1));
    analytics.set_current_funding_rate_8h(-0.05);
    ASSERT_NEAR(analytics.risk_view().current_funding_8h_rate,
                0.05, 1e-12);

    deliver_valid_event(analytics, std::make_shared<funding_event>(
        epoch_ms(2), "PERP", 0.0, -1.0));
    EXPECT_NEAR(analytics.risk_view().current_funding_8h_rate,
                0.05, 1e-12);
}
