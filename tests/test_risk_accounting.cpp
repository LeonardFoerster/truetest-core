// R3 Phase 2 — mark-to-market risk snapshot construction.
//
// Everything here goes through truetest::risk::build_risk_view, the single
// place the authoritative views are assembled from the position ledger, the
// open-order ledger, and timestamped marks. Enforcement lives in
// test_risk_manager.cpp; this file proves the inputs are right.

#include <gtest/gtest.h>

#include "risk/risk_accounting.h"

#include <chrono>
#include <cmath>
#include <string>
#include <unordered_map>

using truetest::risk::build_risk_view;
using truetest::risk::classify_mark;

namespace {

auto epoch_ms(std::int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

order_event make_order(std::uint64_t id, const std::string& symbol,
                       order_side side, double qty, double price = 100.0)
{
    order_event o(epoch_ms(0), symbol, order_type::limit, side, qty, price);
    o.set_order_id(id);
    return o;
}

void open_order(OrderTracker& ledger, std::uint64_t id, const std::string& sym,
                order_side side, double qty, double price = 100.0)
{
    const auto o = make_order(id, sym, side, qty, price);
    ledger.register_order(o);
    ledger.set_status(id, order_status::open);
}

void fill_position(portfolio& port, const std::string& sym, order_side side,
                   double qty, double price, std::uint64_t order_id = 1)
{
    port.on_fill(fill_event(epoch_ms(0), sym, order_id, side, qty, price, 0.0));
}

// Simple in-test mark store mirroring the engine's symbol -> mark_point map.
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
// Mark-to-market
// ---------------------------------------------------------------------------

TEST(RiskAccounting, PositionExposureIsMarkTimesQuantity)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    fill_position(port, "BTCUSDT", order_side::buy, 2.0, /*price=*/100.0);
    mk.set("BTCUSDT", 150.0);

    const auto snap = build("BTCUSDT", port, ledger, mk);
    EXPECT_DOUBLE_EQ(snap.instrument.position_qty, 2.0);
    EXPECT_DOUBLE_EQ(snap.instrument.mark_price, 150.0);
    EXPECT_DOUBLE_EQ(snap.instrument.position_notional, 300.0);
    EXPECT_DOUBLE_EQ(snap.portfolio.gross_exposure, 300.0);
    EXPECT_DOUBLE_EQ(snap.portfolio.net_exposure, 300.0);
    EXPECT_EQ(snap.instrument.mark_state, mark_quality::valid);
}

// Same inventory, new mark: exposure moves without any fill.
TEST(RiskAccounting, ExposureFollowsTheMarkWithoutAFill)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    fill_position(port, "BTCUSDT", order_side::buy, 2.0, 100.0);

    mk.set("BTCUSDT", 100.0);
    EXPECT_DOUBLE_EQ(build("BTCUSDT", port, ledger, mk).instrument.position_notional, 200.0);

    mk.set("BTCUSDT", 250.0);
    const auto after = build("BTCUSDT", port, ledger, mk);
    EXPECT_DOUBLE_EQ(after.instrument.position_notional, 500.0);
    EXPECT_DOUBLE_EQ(after.instrument.position_qty, 2.0);   // no fill happened
    EXPECT_DOUBLE_EQ(after.instrument.unrealized_pnl, 500.0 - 200.0);
}

// Cost basis moves (averaging in) while the mark stays put: exposure must
// track the mark, not the basis.
TEST(RiskAccounting, ExposureIgnoresCostBasisChanges)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);

    fill_position(port, "BTCUSDT", order_side::buy, 1.0, /*price=*/10.0, 1);
    const auto cheap = build("BTCUSDT", port, ledger, mk);

    portfolio expensive_port(10'000.0);
    fill_position(expensive_port, "BTCUSDT", order_side::buy, 1.0, /*price=*/900.0, 2);
    const auto expensive = build("BTCUSDT", expensive_port, ledger, mk);

    // Wildly different cost basis, identical quantity and mark → identical
    // current exposure. Only the P&L split differs.
    EXPECT_DOUBLE_EQ(cheap.instrument.position_notional, 100.0);
    EXPECT_DOUBLE_EQ(expensive.instrument.position_notional, 100.0);
    EXPECT_DOUBLE_EQ(cheap.instrument.position_notional,
                     expensive.instrument.position_notional);
    EXPECT_NE(cheap.instrument.unrealized_pnl, expensive.instrument.unrealized_pnl);
}

TEST(RiskAccounting, ShortPositionExposureIsAbsolute)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    fill_position(port, "BTCUSDT", order_side::sell, 3.0, 100.0);
    mk.set("BTCUSDT", 120.0);

    const auto snap = build("BTCUSDT", port, ledger, mk);
    EXPECT_DOUBLE_EQ(snap.instrument.position_qty, -3.0);
    EXPECT_DOUBLE_EQ(snap.instrument.position_notional, 360.0);
    EXPECT_DOUBLE_EQ(snap.portfolio.gross_exposure, 360.0);
    EXPECT_DOUBLE_EQ(snap.portfolio.net_exposure, -360.0);
}

// ---------------------------------------------------------------------------
// Pending orders / worst case
// ---------------------------------------------------------------------------

TEST(RiskAccounting, FlatPlusOpenBuy)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);
    open_order(ledger, 1, "BTCUSDT", order_side::buy, 5.0);

    const auto v = build("BTCUSDT", port, ledger, mk).instrument;
    EXPECT_DOUBLE_EQ(v.position_qty, 0.0);
    EXPECT_DOUBLE_EQ(v.open_buy_qty, 5.0);
    EXPECT_DOUBLE_EQ(v.open_buy_notional, 500.0);
    EXPECT_EQ(v.open_order_count, 1u);
    EXPECT_DOUBLE_EQ(v.worst_case_long_qty, 5.0);
    EXPECT_DOUBLE_EQ(v.worst_case_short_qty, 0.0);
    EXPECT_DOUBLE_EQ(v.worst_case_long_notional, 500.0);
}

TEST(RiskAccounting, FlatPlusOpenSell)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);
    open_order(ledger, 1, "BTCUSDT", order_side::sell, 4.0);

    const auto v = build("BTCUSDT", port, ledger, mk).instrument;
    EXPECT_DOUBLE_EQ(v.open_sell_qty, 4.0);
    EXPECT_DOUBLE_EQ(v.open_sell_notional, 400.0);
    EXPECT_DOUBLE_EQ(v.worst_case_short_qty, -4.0);
    EXPECT_DOUBLE_EQ(v.worst_case_short_notional, 400.0);
    EXPECT_DOUBLE_EQ(v.worst_case_long_qty, 0.0);
}

TEST(RiskAccounting, LongPlusOpenBuyAndOpenSell)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 10.0);
    fill_position(port, "BTCUSDT", order_side::buy, 6.0, 10.0);
    open_order(ledger, 10, "BTCUSDT", order_side::buy, 4.0);
    open_order(ledger, 11, "BTCUSDT", order_side::sell, 2.0);

    const auto v = build("BTCUSDT", port, ledger, mk).instrument;
    EXPECT_DOUBLE_EQ(v.position_qty, 6.0);
    EXPECT_DOUBLE_EQ(v.worst_case_long_qty, 10.0);   // 6 + 4
    EXPECT_DOUBLE_EQ(v.worst_case_short_qty, 4.0);   // 6 - 2
    EXPECT_DOUBLE_EQ(v.worst_case_long_notional, 100.0);
    EXPECT_DOUBLE_EQ(v.worst_case_short_notional, 40.0);
    EXPECT_EQ(v.open_order_count, 2u);
}

TEST(RiskAccounting, ShortPlusOpenSellAndOpenBuy)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 10.0);
    fill_position(port, "BTCUSDT", order_side::sell, 6.0, 10.0);
    open_order(ledger, 10, "BTCUSDT", order_side::sell, 3.0);
    open_order(ledger, 11, "BTCUSDT", order_side::buy, 1.0);

    const auto v = build("BTCUSDT", port, ledger, mk).instrument;
    EXPECT_DOUBLE_EQ(v.position_qty, -6.0);
    EXPECT_DOUBLE_EQ(v.worst_case_short_qty, -9.0);  // -6 - 3
    EXPECT_DOUBLE_EQ(v.worst_case_long_qty, -5.0);   // -6 + 1
    EXPECT_DOUBLE_EQ(v.worst_case_short_notional, 90.0);
    EXPECT_DOUBLE_EQ(v.worst_case_long_notional, 50.0);
}

TEST(RiskAccounting, PartialFillReducesPendingAndMovesPosition)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);
    open_order(ledger, 1, "BTCUSDT", order_side::buy, 10.0);

    fill_event partial(epoch_ms(0), "BTCUSDT", 1, order_side::buy, 4.0, 100.0,
                       0.0, /*remaining=*/6.0);
    ledger.on_fill(partial);
    port.on_fill(partial);

    const auto v = build("BTCUSDT", port, ledger, mk).instrument;
    EXPECT_DOUBLE_EQ(v.position_qty, 4.0);
    EXPECT_DOUBLE_EQ(v.open_buy_qty, 6.0);
    EXPECT_DOUBLE_EQ(v.worst_case_long_qty, 10.0);
    EXPECT_EQ(v.open_order_count, 1u);
}

TEST(RiskAccounting, CancelRemovesTheRemainder)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);
    open_order(ledger, 1, "BTCUSDT", order_side::buy, 10.0);
    ledger.on_fill(fill_event(epoch_ms(0), "BTCUSDT", 1, order_side::buy, 4.0,
                              100.0, 0.0, 6.0));

    ledger.set_status(1, order_status::cancelled);
    const auto v = build("BTCUSDT", port, ledger, mk).instrument;
    EXPECT_DOUBLE_EQ(v.open_buy_qty, 0.0);
    EXPECT_EQ(v.open_order_count, 0u);
    EXPECT_DOUBLE_EQ(v.worst_case_long_qty, v.position_qty);
}

TEST(RiskAccounting, RejectAndExpireRemovePendingRisk)
{
    for (const auto terminal : {order_status::rejected, order_status::expired})
    {
        portfolio port(10'000.0);
        OrderTracker ledger;
        marks mk;
        mk.set("BTCUSDT", 100.0);
        open_order(ledger, 1, "BTCUSDT", order_side::buy, 7.0);
        ASSERT_DOUBLE_EQ(build("BTCUSDT", port, ledger, mk).instrument.open_buy_qty, 7.0);

        ledger.set_status(1, terminal);
        const auto v = build("BTCUSDT", port, ledger, mk).instrument;
        EXPECT_DOUBLE_EQ(v.open_buy_qty, 0.0) << to_string(terminal);
        EXPECT_DOUBLE_EQ(v.worst_case_long_qty, 0.0) << to_string(terminal);
        EXPECT_EQ(v.open_order_count, 0u) << to_string(terminal);
    }
}

TEST(RiskAccounting, PortfolioAggregatesEveryInstrument)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("BTCUSDT", 100.0);
    mk.set("ETHUSDT", 50.0);
    fill_position(port, "BTCUSDT", order_side::buy, 2.0, 100.0, 1);
    fill_position(port, "ETHUSDT", order_side::sell, 4.0, 50.0, 2);
    open_order(ledger, 10, "BTCUSDT", order_side::buy, 1.0);

    const auto snap = build("BTCUSDT", port, ledger, mk);
    EXPECT_DOUBLE_EQ(snap.portfolio.gross_exposure, 200.0 + 200.0);
    EXPECT_DOUBLE_EQ(snap.portfolio.net_exposure, 200.0 - 200.0);
    // BTC worst case = (2+1)*100 = 300; ETH worst case = 4*50 = 200.
    EXPECT_DOUBLE_EQ(snap.portfolio.worst_case_gross_exposure, 300.0 + 200.0);
    EXPECT_EQ(snap.portfolio.open_order_count, 1u);
    EXPECT_TRUE(snap.ledger_authoritative);
}

TEST(RiskAccounting, SymbolWithOnlyOpenOrdersStillAggregates)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("ETHUSDT", 50.0);
    open_order(ledger, 1, "ETHUSDT", order_side::buy, 3.0);

    // No position was ever filled on ETHUSDT, so it is absent from the
    // portfolio map — the ledger pass must still surface it.
    const auto snap = build("BTCUSDT", port, ledger, mk);
    EXPECT_DOUBLE_EQ(snap.portfolio.worst_case_gross_exposure, 150.0);
    EXPECT_EQ(snap.portfolio.open_order_count, 1u);
}

// ---------------------------------------------------------------------------
// Data quality
// ---------------------------------------------------------------------------

TEST(RiskAccounting, MarkClassification)
{
    const auto now = epoch_ms(10'000);
    EXPECT_EQ(classify_mark(mark_point{}, now, 1000), mark_quality::missing);
    EXPECT_EQ(classify_mark(mark_point{-1.0, epoch_ms(9'900)}, now, 1000),
              mark_quality::missing);
    EXPECT_EQ(classify_mark(mark_point{100.0, epoch_ms(9'900)}, now, 1000),
              mark_quality::valid);
    EXPECT_EQ(classify_mark(mark_point{100.0, epoch_ms(8'000)}, now, 1000),
              mark_quality::stale);
    // Budget disabled → never stale.
    EXPECT_EQ(classify_mark(mark_point{100.0, epoch_ms(0)}, now, 0),
              mark_quality::valid);
    // No observation timestamp → age unknown, cannot claim stale.
    EXPECT_EQ(classify_mark(mark_point{100.0, {}}, now, 1000),
              mark_quality::valid);
}

TEST(RiskAccounting, StaleMarkIsFlaggedAndCounted)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    fill_position(port, "BTCUSDT", order_side::buy, 2.0, 100.0);
    // Non-zero observation stamp: an epoch-zero timestamp is the repository's
    // "never observed" sentinel, and an unknown age must not be called stale.
    mk.set("BTCUSDT", 100.0, /*ts_ms=*/1'000);

    const auto snap = build("BTCUSDT", port, ledger, mk, /*now_ms=*/6'000,
                            /*max_age_ms=*/1'000);
    EXPECT_EQ(snap.instrument.mark_state, mark_quality::stale);
    EXPECT_EQ(snap.instrument.mark_age_ms, 5'000);
    EXPECT_EQ(snap.portfolio.stale_marks, 1u);
    // A stale mark still has a price: it is degraded, not absent.
    EXPECT_DOUBLE_EQ(snap.instrument.mark_price, 100.0);
}

TEST(RiskAccounting, MissingMarkMakesEquityUnknowable)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;   // deliberately empty
    fill_position(port, "BTCUSDT", order_side::buy, 2.0, 100.0);

    const auto snap = build("BTCUSDT", port, ledger, mk);
    EXPECT_EQ(snap.instrument.mark_state, mark_quality::missing);
    EXPECT_DOUBLE_EQ(snap.instrument.mark_price, 0.0);
    EXPECT_EQ(snap.portfolio.positions_without_usable_mark, 1u);
    EXPECT_FALSE(std::isfinite(snap.portfolio.equity));
    EXPECT_FALSE(std::isfinite(snap.equity));
}

TEST(RiskAccounting, FreshMarkGivesCompleteEquityAndPnlSplit)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    fill_position(port, "BTCUSDT", order_side::buy, 2.0, /*price=*/100.0);
    mk.set("BTCUSDT", 130.0, /*ts_ms=*/900);

    const auto snap = build("BTCUSDT", port, ledger, mk, /*now_ms=*/1'000,
                            /*max_age_ms=*/1'000);
    EXPECT_EQ(snap.instrument.mark_state, mark_quality::valid);
    EXPECT_EQ(snap.instrument.mark_age_ms, 100);
    EXPECT_EQ(snap.portfolio.stale_marks, 0u);
    // cash 10000 - 200 = 9800; + 2 * 130 = 10060
    EXPECT_DOUBLE_EQ(snap.portfolio.equity, 10'060.0);
    EXPECT_DOUBLE_EQ(snap.portfolio.unrealized_pnl, 60.0);
    EXPECT_DOUBLE_EQ(snap.portfolio.realized_pnl, 0.0);
}

TEST(RiskAccounting, CandidateSymbolWithoutHistoryStillGetsAView)
{
    portfolio port(10'000.0);
    OrderTracker ledger;
    marks mk;
    mk.set("NEWUSDT", 42.0);

    const auto snap = build("NEWUSDT", port, ledger, mk);
    EXPECT_EQ(snap.instrument.mark_state, mark_quality::valid);
    EXPECT_DOUBLE_EQ(snap.instrument.mark_price, 42.0);
    EXPECT_DOUBLE_EQ(snap.instrument.position_qty, 0.0);
    EXPECT_TRUE(snap.instrument.exposure_tracked);
}
