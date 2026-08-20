// R3 — end-to-end proof that the engine's risk decisions are driven by the
// authoritative ledger rather than by analytics counters or cost basis.
//
// The unit suites (test_order_ledger / test_risk_accounting /
// test_risk_enforcement) exercise the components in isolation; this file runs
// the real engine pipeline and asserts the wiring: ledger registration on
// route, per-symbol pending exposure, worst-case hard-inventory blocking, EOS
// expiry as its own terminal state, and mark-to-market portfolio exposure.

#include <gtest/gtest.h>

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "execution/latency_model.h"
#include "strategy/strategy_interface.h"

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

namespace {

struct SilenceCout {
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceCout() : orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~SilenceCout() { std::cout.rdbuf(orig); }
};

// Emits one market buy of `qty` per bar.
class BarBuyer : public IStrategy
{
public:
    explicit BarBuyer(double qty) : qty_(qty) {}
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        ++calls_;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy,
                           qty_, mkt.get_close());
    }
    void set_position_open(const std::string&, bool) override {}
    int calls() const noexcept { return calls_; }
private:
    double qty_;
    int calls_ = 0;
};

// Buys once, sells the same size once, then stays quiet. Used to show that
// risk-reducing orders survive a hard-inventory breach.
class BuyThenSellOnce : public IStrategy
{
public:
    explicit BuyThenSellOnce(double qty) : qty_(qty) {}
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        const int call = calls_++;
        if (call > 1)
            return std::nullopt;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market,
                           call == 0 ? order_side::buy : order_side::sell,
                           qty_, mkt.get_close());
    }
    void set_position_open(const std::string&, bool) override {}
private:
    double qty_;
    int calls_ = 0;
};

engine_config make_cfg()
{
    engine_config cfg;
    cfg.seed = 1;
    cfg.initial_balance = 100000.0;
    cfg.mm_levels_per_side = 1;
    cfg.mm_base_spread_pct = 0.002;
    cfg.mm_vol_spread_mult = 0.0;
    cfg.show_progress = false;
    // Isolate the risk path: platform-default protective SL/TP would emit
    // extra exit orders (and flatten inventory) on every bar, which is
    // correct behaviour but hides what these tests are asserting.
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    cfg.exit_defaults.sl_pct = 0.0;
    cfg.exit_defaults.tp_pct = 0.0;
    return cfg;
}

std::shared_ptr<data_handler> make_bars(
    const std::vector<std::array<double, 4>>& ohlc,
    const std::string& symbol = "TEST")
{
    auto dh = std::make_shared<data_handler>();
    for (const auto& b : ohlc)
        dh->load_into_queue("2024-01-01", symbol, b[0], b[1], b[2], b[3], 1000);
    return dh;
}

const std::vector<std::array<double, 4>> flat_bars = {
    {100, 101, 99, 100}, {100, 101, 99, 100}, {100, 101, 99, 100},
    {100, 101, 99, 100}, {100, 101, 99, 100},
};

// Net position for `symbol` derived from the authoritative ledger's filled
// quantities. Deliberately not read from a portfolio accessor: the point of
// these tests is that ledger and portfolio agree.
double ledger_net_position(const engine& eng, const std::string& symbol)
{
    const auto& ledger = eng.get_order_tracker();
    double net = 0.0;
    ledger.for_each_order([&](const order_ledger_entry& e) {
        if (ledger.symbol_of(e) != symbol)
            return;
        net += (e.side == order_side::buy) ? e.filled_qty : -e.filled_qty;
    });
    return net;
}

// Count of ledger entries in a given terminal state.
std::size_t count_status(const engine& eng, order_status status)
{
    std::size_t n = 0;
    eng.get_order_tracker().for_each_order(
        [&](const order_ledger_entry& e) { if (e.status == status) ++n; });
    return n;
}

// End-of-run open inventory as analytics reports it (mark-to-market).
const open_position_report* reported_position(const AnalyticsReport& r,
                                              const std::string& symbol)
{
    for (const auto& p : r.open_positions)
        if (p.symbol == symbol) return &p;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------

// Every routed order must appear in the ledger with its real quantity, not
// merely as a status flag.
TEST(EngineRiskLedger, RoutedOrdersCarryQuantityInTheLedger)
{
    SilenceCout quiet;
    auto dh = make_bars(flat_bars);
    auto cfg = make_cfg();
    auto strat = std::make_shared<BarBuyer>(2.0);

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    const auto& ledger = eng.get_order_tracker();
    ASSERT_EQ(strat->calls(), 5);
    // One candidate per bar, all five recorded — including the last one, which
    // the default one-bar execution delay leaves without a future observation
    // and which therefore expires at end of stream.
    EXPECT_EQ(ledger.orders_seen(), 5u);
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("TEST").open_buy_qty, 0.0);

    std::size_t filled = 0, expired = 0;
    ledger.for_each_order([&](const order_ledger_entry& e) {
        EXPECT_EQ(ledger.symbol_of(e), "TEST");
        EXPECT_DOUBLE_EQ(e.original_qty, 2.0);
        EXPECT_TRUE(order_status_is_terminal(e.status)) << to_string(e.status);
        // Terminal, so nothing of it can still fill — whatever the entry's
        // own original-minus-filled arithmetic says.
        EXPECT_DOUBLE_EQ(ledger.pending_qty(e.order_id), 0.0);
        if (e.status == order_status::filled)
        {
            ++filled;
            EXPECT_DOUBLE_EQ(e.filled_qty, 2.0);
            EXPECT_DOUBLE_EQ(e.remaining_qty(), 0.0);
        }
        else if (e.status == order_status::expired)
        {
            ++expired;
            EXPECT_DOUBLE_EQ(e.filled_qty, 0.0);
            EXPECT_DOUBLE_EQ(e.remaining_qty(), 2.0)
                << "an expired order keeps its unfilled quantity on record";
        }
    });
    EXPECT_EQ(filled, 4u);
    EXPECT_EQ(expired, 1u);
    EXPECT_NEAR(ledger_net_position(eng, "TEST"), 8.0, 1e-9);
}

// A latency-queued order occupies pending exposure on its symbol for as long
// as it can still fill — this is what makes the worst case meaningful before
// any fill lands.
TEST(EngineRiskLedger, QueuedOrderHoldsPendingExposureUntilItIsTerminal)
{
    SilenceCout quiet;
    auto dh = make_bars(flat_bars);
    auto cfg = make_cfg();
    cfg.risk.max_open_orders = 1;
    // Held across the whole run: the order never reaches the venue.
    cfg.latency_model = std::make_shared<FixedLatencyModel>(std::chrono::hours(1));
    auto strat = std::make_shared<BarBuyer>(3.0);

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    const auto& ledger = eng.get_order_tracker();
    // End of stream expires the never-submitted candidate, releasing both its
    // slot and its pending exposure.
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("TEST").open_buy_qty, 0.0);
    EXPECT_EQ(ledger.open_exposure("TEST").open_order_count, 0u);

    // The four later candidates saw the reserved slot and were rejected.
    EXPECT_EQ(eng.total_audit_rejections(), 4u);

    // R3: the expiry is recorded as `expired`, distinct from an operator
    // cancel. Exactly one order reached the queue.
    EXPECT_EQ(count_status(eng, order_status::expired), 1u);
    EXPECT_EQ(count_status(eng, order_status::rejected), 4u);
}

// The hard inventory limit is applied to the worst case, so it binds through
// the engine even though each individual order is small.
TEST(EngineRiskLedger, HardInventoryLimitBlocksFurtherBuysThroughTheEngine)
{
    SilenceCout quiet;
    auto dh = make_bars(flat_bars);
    auto cfg = make_cfg();
    cfg.risk.max_symbol_inventory_qty = 4.0;
    // Backtest default is soft portfolio limits (reject, never halt).
    auto strat = std::make_shared<BarBuyer>(2.0);

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    // Two buys of 2 reach the cap (worst case 2 then 4, both within it); the
    // next two are refused because the worst case would be 6. The fifth
    // candidate never gets a future bar and expires instead.
    EXPECT_NEAR(ledger_net_position(eng, "TEST"), 4.0, 1e-9);
    EXPECT_EQ(eng.total_audit_rejections(), 2u);
    const auto report = eng.get_analytics().generate_report();
    const auto* pos = reported_position(report, "TEST");
    ASSERT_NE(pos, nullptr);
    EXPECT_NEAR(pos->quantity, 4.0, 1e-9);
}

// ... and a position-reducing order still gets through after the breach.
TEST(EngineRiskLedger, RiskReducingOrderSurvivesHardInventoryBreach)
{
    SilenceCout quiet;
    auto dh = make_bars(flat_bars);
    auto cfg = make_cfg();
    cfg.risk.max_symbol_inventory_qty = 5.0;
    auto strat = std::make_shared<BuyThenSellOnce>(5.0);

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    // The buy takes inventory exactly to the cap; the sell is inventory
    // reducing and must still go through with the limit sitting at its bound.
    EXPECT_NEAR(ledger_net_position(eng, "TEST"), 0.0, 1e-9);
    EXPECT_EQ(eng.total_audit_rejections(), 0u);
}

// Mark-to-market portfolio exposure: the same inventory becomes too large
// purely because the mark rose, with no additional fills.
TEST(EngineRiskLedger, RisingMarkReValuesHeldInventoryAndBlocksIncreases)
{
    SilenceCout quiet;
    // Price triples over the run while the strategy keeps buying 1 unit.
    auto dh = make_bars({{100, 101, 99, 100},
                         {100, 101, 99, 100},
                         {300, 301, 299, 300},
                         {300, 301, 299, 300}});
    auto cfg = make_cfg();
    cfg.risk.max_portfolio_exposure = 450.0;
    auto strat = std::make_shared<BarBuyer>(1.0);

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    // One unit fills at ~100 while the price is low. Once the mark triples,
    // the held unit alone is worth ~300 and the worst case with another unit
    // is ~600 — over the 450 cap, so every later candidate is refused. The
    // entry basis never moves; only the mark does.
    EXPECT_NEAR(ledger_net_position(eng, "TEST"), 1.0, 1e-9);
    EXPECT_EQ(eng.total_audit_rejections(), 2u);

    const auto report = eng.get_analytics().generate_report();
    const auto* pos = reported_position(report, "TEST");
    ASSERT_NE(pos, nullptr);
    EXPECT_NEAR(pos->quantity, 1.0, 1e-9);
    EXPECT_LT(pos->avg_entry, 150.0) << "entry basis stayed at the low price";
    EXPECT_GT(pos->mark, 250.0) << "inventory is reported mark-to-market";
}

// Regression guard for the whole R3 premise: the analytics fill/order
// counters must not be what decides capacity.
TEST(EngineRiskLedger, AnalyticsCountersDivergeFromLedgerWithoutAffectingRisk)
{
    SilenceCout quiet;
    auto dh = make_bars(flat_bars);
    auto cfg = make_cfg();
    auto strat = std::make_shared<BarBuyer>(2.0);

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    const auto report = eng.get_analytics().snapshot();
    const auto& ledger = eng.get_order_tracker();

    // Reporting counters still exist and still count.
    EXPECT_GT(report.total_fills, 0u);
    // The ledger's lifetime order count is its own, independent number: five
    // candidates, of which only four ever produced a fill.
    EXPECT_EQ(ledger.orders_seen(), 5u);
    EXPECT_EQ(report.total_fills, 4u);
    // And the authoritative open-order state is zero regardless of them.
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_EQ(ledger.get_open_orders().size(), 0u);
}
