// R3 Phase 2 — RiskManager enforcement against the authoritative snapshot.
//
// test_risk_accounting.cpp proves the snapshot inputs are correct; this file
// proves the limits act on them: worst-case pending exposure, hard inventory
// limits, inventory-increasing vs risk-reducing classification, stale/missing
// marks, funding, machine-readable rule codes, and the regression guarantee
// that analytics counters can no longer change any decision.

#include <gtest/gtest.h>

#include "analytics/analytics.h"
#include "risk/risk_accounting.h"

#include <chrono>
#include <cmath>
#include <string>
#include <unordered_map>

using truetest::risk::build_risk_view;

namespace {

auto epoch_ms(std::int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

order_event make_order(const std::string& symbol, order_side side, double qty,
                       double price = 100.0, std::uint64_t id = 0)
{
    order_event o(epoch_ms(0), symbol, order_type::limit, side, qty, price);
    o.set_order_id(id);
    return o;
}

void open_order(OrderTracker& ledger, std::uint64_t id, const std::string& sym,
                order_side side, double qty, double price = 100.0)
{
    order_event o(epoch_ms(0), sym, order_type::limit, side, qty, price);
    o.set_order_id(id);
    ledger.register_order(o);
    ledger.set_status(id, order_status::open);
}

void fill_position(portfolio& port, const std::string& sym, order_side side,
                   double qty, double price, std::uint64_t order_id = 1)
{
    port.on_fill(fill_event(epoch_ms(0), sym, order_id, side, qty, price, 0.0));
}

struct marks
{
    std::unordered_map<std::string, mark_point> m;
    void set(const std::string& sym, double px, std::int64_t ts_ms = 0)
    {
        m[sym] = mark_point{px, epoch_ms(ts_ms)};
    }
    mark_point operator()(const std::string& sym) const
    {
        auto it = m.find(sym);
        return (it != m.end()) ? it->second : mark_point{};
    }
};

risk_snapshot build(const std::string& candidate, const portfolio& port,
                    const OrderTracker& ledger, const marks& mk,
                    std::int64_t now_ms = 0, std::int64_t max_age_ms = 0)
{
    risk_snapshot snap;
    build_risk_view(snap, candidate, port, ledger, epoch_ms(now_ms),
                    max_age_ms, mk);
    return snap;
}

} // namespace

// ---------------------------------------------------------------------------
// Inventory-effect classification
// ---------------------------------------------------------------------------

TEST(RiskEnforcement, InventoryEffectClassification)
{
    // Flat: any order opens inventory.
    EXPECT_EQ(classify_inventory_effect(order_side::buy, 1.0, 0.0),
              inventory_effect::increasing);
    EXPECT_EQ(classify_inventory_effect(order_side::sell, 1.0, 0.0),
              inventory_effect::increasing);
    // Long.
    EXPECT_EQ(classify_inventory_effect(order_side::buy, 1.0, 5.0),
              inventory_effect::increasing);
    EXPECT_EQ(classify_inventory_effect(order_side::sell, 5.0, 5.0),
              inventory_effect::reducing);
    EXPECT_EQ(classify_inventory_effect(order_side::sell, 6.0, 5.0),
              inventory_effect::increasing);   // flip = new opposite exposure
    // Short.
    EXPECT_EQ(classify_inventory_effect(order_side::sell, 1.0, -5.0),
              inventory_effect::increasing);
    EXPECT_EQ(classify_inventory_effect(order_side::buy, 5.0, -5.0),
              inventory_effect::reducing);
    EXPECT_EQ(classify_inventory_effect(order_side::buy, 6.0, -5.0),
              inventory_effect::increasing);
    // Degenerate.
    EXPECT_EQ(classify_inventory_effect(order_side::buy, 0.0, 5.0),
              inventory_effect::neutral);
}

// ---------------------------------------------------------------------------
// Candidate checked against current + pending
// ---------------------------------------------------------------------------

TEST(RiskEnforcement, CandidateIsCheckedAgainstExistingOpenBuys)
{
    risk_limits lim;
    lim.max_position_value = 1'000.0;   // 10 units at a mark of 100
    RiskManager rm(lim);

    portfolio port(1'000'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);

    // Three resting buys of 3 each: 9 units pending, still inside the cap.
    open_order(ledger, 1, "BTCUSDT", order_side::buy, 3.0);
    open_order(ledger, 2, "BTCUSDT", order_side::buy, 3.0);
    open_order(ledger, 3, "BTCUSDT", order_side::buy, 3.0);

    auto snap = build("BTCUSDT", port, ledger, mk);
    // In isolation a 4th 3-unit buy is only 300 of notional; aggregated with
    // the resting orders it is 12 units = 1200 > 1000.
    const auto candidate = make_order("BTCUSDT", order_side::buy, 3.0);
    risk_rule rule = risk_rule::none;
    EXPECT_EQ(rm.check_order(candidate, port, snap, 3, &rule), risk_action::reject);
    EXPECT_EQ(rule, risk_rule::position_limit);

    // Same candidate with no resting orders passes — proof the rejection came
    // from the aggregation, not from the candidate alone.
    OrderTracker empty_ledger;
    auto lone = build("BTCUSDT", port, empty_ledger, mk);
    EXPECT_EQ(rm.check_order(candidate, port, lone, 0), risk_action::pass);
}

TEST(RiskEnforcement, CandidateSellAggregatesWithExistingOpenSells)
{
    risk_limits lim;
    lim.max_position_value = 1'000.0;
    RiskManager rm(lim);

    portfolio port(1'000'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);
    open_order(ledger, 1, "BTCUSDT", order_side::sell, 8.0);

    auto snap = build("BTCUSDT", port, ledger, mk);
    risk_rule rule = risk_rule::none;
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::sell, 3.0),
                             port, snap, 1, &rule),
              risk_action::reject);
    EXPECT_EQ(rule, risk_rule::position_limit);
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::sell, 1.0),
                             port, snap, 1),
              risk_action::pass);
}

// The same order must not be counted twice when it is already staged (a
// pending stop converting to a market order keeps its id and its slot).
TEST(RiskEnforcement, AlreadyStagedOrderIsNotDoubleCounted)
{
    risk_limits lim;
    lim.max_symbol_inventory_qty = 5.0;
    RiskManager rm(lim);

    portfolio port(1'000'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);
    open_order(ledger, 42, "BTCUSDT", order_side::buy, 5.0);

    auto snap = build("BTCUSDT", port, ledger, mk);
    const auto staged = make_order("BTCUSDT", order_side::buy, 5.0, 100.0, 42);

    // Naively: 5 pending + 5 candidate = 10 > 5 → reject.
    EXPECT_EQ(rm.check_order(staged, port, snap, 0), risk_action::reject);

    // The engine removes the staged order's own remaining quantity from its
    // side before the check (see OrderIntentProcessor::process).
    snap.instrument.open_buy_qty -= ledger.pending_qty(42);
    EXPECT_EQ(rm.check_order(staged, port, snap, 0), risk_action::pass);
}

// ---------------------------------------------------------------------------
// Hard inventory limit + risk-reducing exemption
// ---------------------------------------------------------------------------

TEST(RiskEnforcement, LongAtHardLimitBlocksBuyAllowsSell)
{
    risk_limits lim;
    lim.max_symbol_inventory_qty = 10.0;
    RiskManager rm(lim);

    portfolio port(1'000'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);
    fill_position(port, "BTCUSDT", order_side::buy, 10.0, 100.0);
    auto snap = build("BTCUSDT", port, ledger, mk);

    risk_rule rule = risk_rule::none;
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 1.0),
                             port, snap, 0, &rule),
              risk_action::reject);
    EXPECT_EQ(rule, risk_rule::hard_inventory_limit);
    EXPECT_STREQ(to_string(rule), "hard_inventory_limit");

    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::sell, 4.0),
                             port, snap, 0),
              risk_action::pass);
}

TEST(RiskEnforcement, ShortAtHardLimitBlocksSellAllowsBuy)
{
    risk_limits lim;
    lim.max_symbol_inventory_qty = 10.0;
    RiskManager rm(lim);

    portfolio port(1'000'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);
    fill_position(port, "BTCUSDT", order_side::sell, 10.0, 100.0);
    auto snap = build("BTCUSDT", port, ledger, mk);

    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::sell, 1.0),
                             port, snap, 0),
              risk_action::reject);
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 4.0),
                             port, snap, 0),
              risk_action::pass);
}

TEST(RiskEnforcement, HardInventoryLimitCountsPendingOrders)
{
    risk_limits lim;
    lim.max_symbol_inventory_qty = 10.0;
    RiskManager rm(lim);

    portfolio port(1'000'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);
    fill_position(port, "BTCUSDT", order_side::buy, 6.0, 100.0);
    open_order(ledger, 1, "BTCUSDT", order_side::buy, 3.0);
    auto snap = build("BTCUSDT", port, ledger, mk);

    // 6 held + 3 resting + 1 candidate = 10 → exactly at the limit, allowed.
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 1.0),
                             port, snap, 1),
              risk_action::pass);
    // 6 + 3 + 2 = 11 → over.
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 2.0),
                             port, snap, 1),
              risk_action::reject);
}

TEST(RiskEnforcement, HardInventoryLimitBoundaries)
{
    risk_limits lim;
    lim.max_symbol_inventory_qty = 10.0;
    RiskManager rm(lim);

    portfolio port(1'000'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);
    auto snap = build("BTCUSDT", port, ledger, mk);

    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 9.999),
                             port, snap, 0), risk_action::pass);
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 10.0),
                             port, snap, 0), risk_action::pass);
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 10.001),
                             port, snap, 0), risk_action::reject);
}

// A reducing order is exempt from the inventory cap but not from every rule:
// the open-order capacity limit has no reduce-only exemption.
TEST(RiskEnforcement, ReducingOrderStillFacesCapacityLimit)
{
    risk_limits lim;
    lim.max_symbol_inventory_qty = 1.0;
    lim.max_open_orders = 2;
    RiskManager rm(lim);

    portfolio port(1'000'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);
    fill_position(port, "BTCUSDT", order_side::buy, 10.0, 100.0);
    auto snap = build("BTCUSDT", port, ledger, mk);

    const auto reduce = make_order("BTCUSDT", order_side::sell, 4.0);
    EXPECT_EQ(rm.check_order(reduce, port, snap, 1), risk_action::pass);

    risk_rule rule = risk_rule::none;
    EXPECT_EQ(rm.check_order(reduce, port, snap, 2, &rule), risk_action::reject);
    EXPECT_EQ(rule, risk_rule::max_open_orders);
}

// ---------------------------------------------------------------------------
// Portfolio aggregation
// ---------------------------------------------------------------------------

TEST(RiskEnforcement, PortfolioExposureAggregatesAcrossSymbols)
{
    risk_limits lim;
    lim.max_position_value = 1e9;
    lim.max_portfolio_exposure = 1'000.0;
    RiskManager rm(lim);

    portfolio port(1'000'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);
    mk.set("ETHUSDT", 100.0);
    fill_position(port, "ETHUSDT", order_side::buy, 7.0, 100.0, 1);   // 700
    auto snap = build("BTCUSDT", port, ledger, mk);

    // 700 (ETH) + 200 (candidate BTC) = 900 → inside.
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 2.0),
                             port, snap, 0), risk_action::pass);
    // 700 + 400 = 1100 → over.
    risk_rule rule = risk_rule::none;
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 4.0),
                             port, snap, 0, &rule), risk_action::reject);
    EXPECT_EQ(rule, risk_rule::portfolio_exposure);
}

TEST(RiskEnforcement, PortfolioExposureUsesMarksNotCostBasis)
{
    risk_limits lim;
    lim.max_portfolio_exposure = 1'000.0;
    RiskManager rm(lim);

    portfolio port(1'000'000.0);
    OrderTracker ledger;
    marks mk;
    // Bought ETH cheaply (cost basis 100) but it now marks at 900.
    fill_position(port, "ETHUSDT", order_side::buy, 1.0, /*price=*/100.0, 1);
    mk.set("ETHUSDT", 900.0);
    mk.set("BTCUSDT", 100.0);
    auto snap = build("BTCUSDT", port, ledger, mk);

    // Cost-basis accounting would say 100 + 200 = 300 (pass). Mark-to-market
    // says 900 + 200 = 1100 → reject.
    EXPECT_DOUBLE_EQ(snap.portfolio.gross_exposure, 900.0);
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 2.0),
                             port, snap, 0), risk_action::reject);
}

// ---------------------------------------------------------------------------
// Data quality
// ---------------------------------------------------------------------------

TEST(RiskEnforcement, MissingMarkRefusesNewInventoryAllowsReduction)
{
    risk_limits lim;
    lim.max_position_value = 1e6;
    RiskManager rm(lim);

    portfolio port(1'000'000.0);
    OrderTracker ledger;
    marks mk;   // no marks at all
    fill_position(port, "BTCUSDT", order_side::buy, 5.0, 100.0);
    auto snap = build("BTCUSDT", port, ledger, mk);

    // A market order carries no price of its own either → nothing to value.
    order_event blind(epoch_ms(0), "BTCUSDT", order_type::market,
                      order_side::buy, 1.0, /*price=*/0.0);
    risk_rule rule = risk_rule::none;
    EXPECT_EQ(rm.check_order(blind, port, snap, 0, &rule), risk_action::reject);
    EXPECT_EQ(rule, risk_rule::stale_mark);

    order_event exit_blind(epoch_ms(0), "BTCUSDT", order_type::market,
                           order_side::sell, 5.0, /*price=*/0.0);
    EXPECT_EQ(rm.check_order(exit_blind, port, snap, 0), risk_action::pass);
}

TEST(RiskEnforcement, StaleMarkBlocksNewInventoryWhenFreshnessIsRequired)
{
    risk_limits lim;
    lim.max_mark_age_ms = 1'000;
    lim.require_fresh_mark = true;
    RiskManager rm(lim);

    portfolio port(1'000'000.0);
    OrderTracker ledger;
    marks mk;
    fill_position(port, "BTCUSDT", order_side::buy, 5.0, 100.0);
    mk.set("BTCUSDT", 100.0, /*ts_ms=*/1'000);

    auto stale = build("BTCUSDT", port, ledger, mk, /*now_ms=*/9'000, 1'000);
    ASSERT_EQ(stale.instrument.mark_state, mark_quality::stale);
    risk_rule rule = risk_rule::none;
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 1.0),
                             port, stale, 0, &rule), risk_action::reject);
    EXPECT_EQ(rule, risk_rule::stale_mark);
    // De-risking stays possible under bad market data.
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::sell, 2.0),
                             port, stale, 0), risk_action::pass);

    auto fresh = build("BTCUSDT", port, ledger, mk, /*now_ms=*/1'500, 1'000);
    ASSERT_EQ(fresh.instrument.mark_state, mark_quality::valid);
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 1.0),
                             port, fresh, 0), risk_action::pass);
}

TEST(RiskEnforcement, StaleMarkIsStillUsableWhenFreshnessIsNotRequired)
{
    risk_limits lim;
    lim.max_mark_age_ms = 1'000;
    lim.require_fresh_mark = false;      // research/backtest default
    lim.max_position_value = 400.0;
    RiskManager rm(lim);

    portfolio port(1'000'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0, /*ts_ms=*/1'000);
    auto stale = build("BTCUSDT", port, ledger, mk, /*now_ms=*/9'000, 1'000);
    ASSERT_EQ(stale.instrument.mark_state, mark_quality::stale);

    // The degraded price is still applied to the limit rather than skipped.
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 3.0),
                             port, stale, 0), risk_action::pass);
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 5.0),
                             port, stale, 0), risk_action::reject);
}

// ---------------------------------------------------------------------------
// Funding
// ---------------------------------------------------------------------------

TEST(RiskEnforcement, FundingLimitBelowAtAndAboveTheBound)
{
    risk_limits lim;
    lim.max_funding_8h_rate = 0.001;
    RiskManager rm(lim);
    portfolio port(1'000'000.0);

    const auto candidate = make_order("BTCUSDT", order_side::buy, 1.0);
    auto with_rate = [&](double rate) {
        risk_snapshot s;
        s.funding_rate_known = true;
        s.current_funding_8h_rate = rate;
        return s;
    };

    auto below = with_rate(0.0009);
    EXPECT_EQ(rm.check_order(candidate, port, below, 0), risk_action::pass);

    auto at = with_rate(0.001);                 // strictly-greater comparison
    EXPECT_EQ(rm.check_order(candidate, port, at, 0), risk_action::pass);

    auto above = with_rate(0.0011);
    risk_rule rule = risk_rule::none;
    EXPECT_EQ(rm.check_order(candidate, port, above, 0, &rule), risk_action::reject);
    EXPECT_EQ(rule, risk_rule::funding_limit);

    auto severe = with_rate(0.01);              // > 1.5x → halt
    EXPECT_EQ(rm.check_order(candidate, port, severe, 0), risk_action::halt);
}

// The pre-R3 gap: with no producer the rate was permanently 0.0, which read
// as "inside the limit" instead of "unknown".
TEST(RiskEnforcement, UnknownFundingRateIsNotTreatedAsInsideTheLimit)
{
    risk_limits lim;
    lim.max_funding_8h_rate = 0.001;
    RiskManager rm(lim);
    portfolio port(1'000'000.0);

    risk_snapshot unknown;
    unknown.funding_rate_known = false;
    unknown.current_funding_8h_rate = 0.05;   // would breach if it were known
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 1.0),
                             port, unknown, 0), risk_action::pass);
}

TEST(RiskEnforcement, FundingSettlementProducesTheRate)
{
    Analytics analytics(10'000.0);
    EXPECT_FALSE(analytics.risk_view().funding_rate_known);

    // Long 2 BTC marked at 1000 → notional 2000. A -1.0 USDT funding fee is a
    // rate of 0.0005 (5 bps per 8h), the venue identity
    // funding_fee = -notional * rate.
    analytics.on_event(std::make_shared<fill_event>(
        epoch_ms(0), "BTCUSDT", 1, order_side::buy, 2.0, 1'000.0,
        0.0, 0.0, 1));
    analytics.on_mark("BTCUSDT", 1'000.0);
    analytics.on_funding(funding_event(epoch_ms(1), "BTCUSDT", 0.0, -1.0));

    const auto view = analytics.risk_view();
    EXPECT_TRUE(view.funding_rate_known);
    EXPECT_NEAR(view.current_funding_8h_rate, 0.0005, 1e-12);

    // And a short receiving funding produces a positive rate too (the payer
    // side is what the cap is written against).
    Analytics shorted(10'000.0);
    shorted.on_event(std::make_shared<fill_event>(
        epoch_ms(0), "BTCUSDT", 1, order_side::sell, 2.0, 1'000.0,
        0.0, 0.0, 1));
    shorted.on_mark("BTCUSDT", 1'000.0);
    shorted.on_funding(funding_event(epoch_ms(1), "BTCUSDT", 0.0, +1.0));
    EXPECT_NEAR(shorted.risk_view().current_funding_8h_rate, 0.0005, 1e-12);
}

TEST(RiskEnforcement, SpotInstrumentNeverAcquiresAFundingRate)
{
    Analytics analytics(10'000.0);
    analytics.on_event(std::make_shared<fill_event>(
        epoch_ms(0), "AAPL", 1, order_side::buy, 2.0, 100.0,
        0.0, 0.0, 1));
    analytics.on_mark("AAPL", 100.0);
    // No funding_event is ever emitted for a spot instrument.
    EXPECT_FALSE(analytics.risk_view().funding_rate_known);
    EXPECT_DOUBLE_EQ(analytics.risk_view().current_funding_8h_rate, 0.0);
}

// ---------------------------------------------------------------------------
// Regression: analytics counters are not risk state
// ---------------------------------------------------------------------------

// risk_snapshot no longer even carries total_orders / total_fills, so this is
// enforced by the type system. The behavioural half: an AnalyticsReport with
// absurd counters must not move any decision.
TEST(RiskEnforcement, AnalyticsCountersCannotChangeADecision)
{
    risk_limits lim;
    lim.max_open_orders = 3;
    RiskManager rm(lim);
    portfolio port(1'000'000.0);
    const auto candidate = make_order("BTCUSDT", order_side::buy, 1.0);

    AnalyticsReport quiet;
    quiet.total_orders = 0;
    quiet.total_fills = 0;
    AnalyticsReport noisy;
    noisy.total_orders = 1'000'000;
    noisy.total_fills = 999'999;

    // Identical authoritative open-order count → identical decision, whatever
    // the reporting counters claim.
    for (std::size_t open_count : {std::size_t{0}, std::size_t{2}, std::size_t{3}})
    {
        EXPECT_EQ(rm.check_order(candidate, port, quiet, open_count),
                  rm.check_order(candidate, port, noisy, open_count))
            << "open_count=" << open_count;
    }
    EXPECT_EQ(rm.check_order(candidate, port, noisy, 2), risk_action::pass);
    EXPECT_EQ(rm.check_order(candidate, port, noisy, 3), risk_action::reject);
}

TEST(RiskEnforcement, RuleCodesAreStableAndDistinct)
{
    const risk_rule all[] = {
        risk_rule::none, risk_rule::max_open_orders, risk_rule::position_limit,
        risk_rule::hard_inventory_limit, risk_rule::portfolio_exposure,
        risk_rule::position_pct_of_equity, risk_rule::invalid_equity,
        risk_rule::stale_mark, risk_rule::drawdown, risk_rule::daily_loss,
        risk_rule::loss_per_trade, risk_rule::trades_per_hour,
        risk_rule::orders_per_minute, risk_rule::spread_limit,
        risk_rule::funding_limit,
    };
    std::unordered_map<std::string, int> seen;
    for (auto r : all)
    {
        const std::string code = to_string(r);
        EXPECT_FALSE(code.empty());
        EXPECT_EQ(code.find(' '), std::string::npos) << code;
        ++seen[code];
    }
    EXPECT_EQ(seen.size(), std::size(all));

    EXPECT_STREQ(to_string(risk_rule::max_open_orders), "max_open_orders");
    EXPECT_STREQ(to_string(risk_rule::position_limit), "position_limit");
    EXPECT_STREQ(to_string(risk_rule::portfolio_exposure), "portfolio_exposure");
    EXPECT_STREQ(to_string(risk_rule::stale_mark), "stale_mark");
    EXPECT_STREQ(to_string(risk_rule::daily_loss), "daily_loss");
    EXPECT_STREQ(to_string(risk_rule::drawdown), "drawdown");
    EXPECT_STREQ(to_string(risk_rule::funding_limit), "funding_limit");
}

TEST(RiskEnforcement, DrawdownAndDailyLossReportTheirRuleCode)
{
    risk_limits lim;
    lim.max_drawdown = 0.10;
    RiskManager rm(lim);
    portfolio port(1'000'000.0);

    risk_snapshot snap;
    snap.max_drawdown = 15.0;
    risk_rule rule = risk_rule::none;
    EXPECT_EQ(rm.check_order(make_order("BTCUSDT", order_side::buy, 1.0),
                             port, snap, 0, &rule), risk_action::halt);
    EXPECT_EQ(rule, risk_rule::drawdown);

    risk_limits daily;
    daily.max_daily_loss = 50.0;
    daily.max_drawdown = 1.0;
    RiskManager daily_rm(daily);
    risk_snapshot post;
    post.has_last_trade = true;
    post.last_trade_pnl = -100.0;
    post.last_trade_seq = 1;
    risk_rule post_rule = risk_rule::none;
    EXPECT_EQ(daily_rm.check_post_fill(
                  fill_event(epoch_ms(0), "BTCUSDT", 1, order_side::sell, 1.0,
                             100.0, 0.0),
                  port, post, &post_rule),
              risk_action::halt);
    EXPECT_EQ(post_rule, risk_rule::daily_loss);
    EXPECT_DOUBLE_EQ(daily_rm.daily_realized_loss(), 100.0);
}

// ---------------------------------------------------------------------------
// VaR removal (R3 decision: case B — no estimator existed, field deleted)
// ---------------------------------------------------------------------------

TEST(RiskEnforcement, NoPortfolioVarSafetyClaimRemains)
{
    // The struct no longer exposes a portfolio-VaR bound. This test exists so
    // reintroducing the field without an estimator fails review loudly: a
    // configurable limit that nothing computes is a false safety claim.
    // (Compile-time proof; the runtime half is that the limits that DO exist
    // are all reachable from a rule code.)
    risk_limits lim;
    EXPECT_DOUBLE_EQ(lim.max_position_value, 1e9);
    EXPECT_DOUBLE_EQ(lim.max_portfolio_exposure, 5e9);
    EXPECT_DOUBLE_EQ(lim.max_symbol_inventory_qty, 0.0);
    EXPECT_EQ(lim.max_mark_age_ms, 0);
    EXPECT_FALSE(lim.require_fresh_mark);

    // realized_vol_1h survives: it has a defined meaning and a producer.
    risk_snapshot snap;
    EXPECT_DOUBLE_EQ(snap.realized_vol_1h, 0.0);
}
