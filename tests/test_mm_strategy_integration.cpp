// R1 integration suite (I01-I08), look-ahead proof, hot-path allocation proof
// and telemetry wiring.
//
// The pipeline exercised here is:
//   canonical L2 event -> market snapshot -> strategy -> quote intents
//   -> pre-trade risk (RiskManager) -> orderbook matcher -> fills
//   -> authoritative ledger -> next strategy input
//
// The ledger is a test double for the order/portfolio state R3 will own; see
// docs/internal/r1-inventory-aware-market-making.md "Known gaps".

#include <gtest/gtest.h>

#include "helpers/alloc_counter.h"
#include "helpers/mm_test_harness.h"

#include "execution/portfolio.h"
#include "orderbook/orderbook.h"
#include "types/order_id.h"
#include "risk/risk_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

using namespace truetest::mm;
using namespace truetest::mm::test;

namespace
{

// ── canonical L2 event -> market snapshot ───────────────────────────────────
// The projection the market-state layer performs; kept in test code because
// the engine-side publisher is a follow-up (see Known gaps).
market_snapshot market_snapshot_from_l2(const l2_snapshot_event& ev,
                                        timestamp_ns event_time_ns,
                                        timestamp_ns receive_time_ns,
                                        std::uint64_t snapshot_id)
{
    market_snapshot m;
    m.event_time_ns = event_time_ns;
    m.receive_time_ns = receive_time_ns;
    m.best_bid = Price::from_double(ev.bid(0).price);
    m.best_ask = Price::from_double(ev.ask(0).price);
    m.best_bid_qty = ev.bid(0).quantity;
    m.best_ask_qty = ev.ask(0).quantity;
    m.sequence_valid = true;
    m.snapshot_id = snapshot_id;
    return m;
}

// ── authoritative inventory double ──────────────────────────────────────────
struct sim_ledger
{
    qty_atoms position = 0;
    qty_atoms pending_buy = 0;
    qty_atoms pending_sell = 0;

    [[nodiscard]] inventory_snapshot snapshot() const
    {
        inventory_snapshot inv;
        inv.signed_base_position = position;
        inv.hard_limit = 0;
        inv.worst_case_position_if_all_buys_fill = position + pending_buy;
        inv.worst_case_position_if_all_sells_fill = position - pending_sell;
        inv.authoritative = true;
        return inv;
    }

    void replace_with(const quote_decision& d)
    {
        pending_buy = total_side_qty(d, order_side::buy);
        pending_sell = total_side_qty(d, order_side::sell);
    }

    void cancel_all()
    {
        pending_buy = 0;
        pending_sell = 0;
    }
};

// Keeps the measured loop's result observable so the optimiser cannot elide
// the call being measured.
volatile std::size_t observable_intent_count = 0;

std::chrono::system_clock::time_point tp_from_ns(timestamp_ns ns)
{
    return std::chrono::system_clock::time_point(std::chrono::nanoseconds(ns));
}

} // namespace

// ── I01 full pipeline ───────────────────────────────────────────────────────
TEST(MMStrategyIntegration, I01_L2ToStrategyToRiskToSimulator)
{
    const l2_level bids[] = {{60000.00, atoms_from_decimal("4.20")},
                             {59999.50, atoms_from_decimal("2.00")}};
    const l2_level asks[] = {{60000.50, atoms_from_decimal("1.40")},
                             {60001.00, atoms_from_decimal("3.00")}};
    const l2_snapshot_event ev(tp_from_ns(base_event_time_ns), "BTCUSDT",
                               bids, 2, asks, 2, 100'000'000ULL);

    const auto market = market_snapshot_from_l2(ev, base_event_time_ns,
                                                base_event_time_ns + 100'000, 1);

    auto strat = make_strategy(default_config());
    const auto ctx = default_context();
    const auto res = strat.evaluate(market, flat_inventory(), ctx);
    ASSERT_TRUE(res.ok());
    const auto& d = res.decision;
    ASSERT_EQ(d.state, mm_state::active);
    ASSERT_FALSE(d.intents.empty());

    // Pre-trade risk sees ordinary order intents.
    RiskManager risk{risk_limits{}};
    portfolio port(1'000'000.0);
    risk_snapshot snap{};
    snap.equity = 1'000'000.0;

    auto book = std::make_shared<orderbook>();
    std::size_t accepted = 0;

    for (std::size_t i = 0; i < d.intents.size(); ++i)
    {
        const auto& q = d.intents[i];
        order_event oe(tp_from_ns(ctx.decision_time_ns), "BTCUSDT", order_type::limit,
                       q.side, static_cast<double>(q.quantity) / static_cast<double>(atoms_per_unit),
                       q.price.to_double(), time_in_force::gtc);
        ASSERT_EQ(risk.check_order(oe, port, snap, accepted), risk_action::pass);
        ++accepted;

        const auto ob_side = (q.side == order_side::buy) ? side::buy : side::sell;
        auto resting = book->create_order(ob_order_type::good_till_cancel,
                                          OrderIdGenerator::next(), ob_side, q.price,
                                          static_cast<quantity>(q.quantity));
        // Post-only quotes must rest, never trade on arrival.
        const auto trades_on_arrival = book->add_external_order(resting);
        EXPECT_TRUE(trades_on_arrival.empty());
    }

    EXPECT_EQ(accepted, d.intents.size());
    EXPECT_EQ(book->size(), d.intents.size());
}

// ── I02 fill changes inventory, next decision skews ─────────────────────────
TEST(MMStrategyIntegration, I02_FillMovesInventoryAndNextDecisionSkews)
{
    auto strat = make_strategy(default_config());
    const auto market = default_market();
    const auto ctx = default_context();

    sim_ledger ledger;
    const auto first = strat.evaluate(market, ledger.snapshot(), ctx).decision;
    ledger.replace_with(first);

    const auto* bid = find_intent(first, order_side::buy, 0);
    ASSERT_NE(bid, nullptr);

    // Full fill of the resting bid.
    ledger.position += bid->quantity;
    ledger.cancel_all();

    const auto second = strat.evaluate(market, ledger.snapshot(), ctx).decision;
    EXPECT_LT(second.reservation_price.raw(), first.reservation_price.raw());
    EXPECT_LT(second.bid_size, first.bid_size);
    EXPECT_GT(second.ask_size, first.ask_size);
    EXPECT_GT(second.inventory_utilization, first.inventory_utilization);
}

// ── I03 partial fill ────────────────────────────────────────────────────────
TEST(MMStrategyIntegration, I03_PartialFillLeavesRemainderInWorstCase)
{
    auto cfg = default_config();
    cfg.quotes.base_size = atoms_from_decimal("0.40");
    auto strat = make_strategy(cfg);
    const auto market = default_market();
    const auto ctx = default_context();

    sim_ledger ledger;
    const auto first = strat.evaluate(market, ledger.snapshot(), ctx).decision;
    const auto* bid = find_intent(first, order_side::buy, 0);
    ASSERT_NE(bid, nullptr);
    ledger.replace_with(first);

    const qty_atoms filled = bid->quantity / 2;
    ledger.position += filled;
    ledger.pending_buy -= filled;

    const auto inv = ledger.snapshot();
    EXPECT_EQ(inv.signed_base_position, filled);
    EXPECT_EQ(inv.worst_case_position_if_all_buys_fill, filled + (bid->quantity - filled));

    const auto second = strat.evaluate(market, inv, ctx).decision;
    EXPECT_LT(second.reservation_price.raw(), first.reservation_price.raw());
    // The remaining live buy still counts against the limit.
    EXPECT_LE(inv.worst_case_position_if_all_buys_fill
                  + total_side_qty(second, order_side::buy),
              cfg.inventory.hard_limit_base);
}

// ── I04 cancel/replace ──────────────────────────────────────────────────────
TEST(MMStrategyIntegration, I04_CancelReplaceUsesUpdatedPendingState)
{
    auto cfg = default_config();
    cfg.quotes.base_size = atoms_from_decimal("0.20");
    auto strat = make_strategy(cfg);
    const auto market = default_market();
    const auto ctx = default_context();

    sim_ledger ledger;
    ledger.position = static_cast<qty_atoms>(
        static_cast<double>(cfg.inventory.hard_limit_base) * 0.6);
    ledger.pending_buy = cfg.inventory.hard_limit_base - ledger.position; // limit reached

    const auto blocked = strat.evaluate(market, ledger.snapshot(), ctx).decision;
    EXPECT_EQ(count_side(blocked, order_side::buy), 0u);
    EXPECT_TRUE(has_reason(blocked, quote_reason::inventory_hard_limit));

    // The execution layer cancels the outstanding buys; the very next
    // decision must see the freed headroom.
    ledger.cancel_all();
    const auto freed = strat.evaluate(market, ledger.snapshot(), ctx).decision;
    EXPECT_GE(count_side(freed, order_side::buy), 1u);
    EXPECT_LE(ledger.position + total_side_qty(freed, order_side::buy),
              cfg.inventory.hard_limit_base);
}

// ── I05 hard limit from several pending buys ────────────────────────────────
TEST(MMStrategyIntegration, I05_PendingBuysAloneCloseTheBuySide)
{
    auto cfg = default_config();
    cfg.levels = 3;
    cfg.quotes.base_size = atoms_from_decimal("0.05");
    auto strat = make_strategy(cfg);
    const auto market = default_market();
    const auto ctx = default_context();

    sim_ledger ledger;
    // Nothing filled yet: position stays flat while live buys accumulate.
    for (int round = 0; round < 40; ++round)
    {
        const auto d = strat.evaluate(market, ledger.snapshot(), ctx).decision;
        const qty_atoms new_buys = total_side_qty(d, order_side::buy);

        // Worst case must never breach the limit, however many rounds ran.
        EXPECT_LE(ledger.pending_buy + new_buys, cfg.inventory.hard_limit_base)
            << "round " << round;

        if (new_buys == 0)
        {
            EXPECT_TRUE(has_reason(d, quote_reason::inventory_hard_limit));
            EXPECT_EQ(d.state, mm_state::reducing_only);
            EXPECT_GE(count_side(d, order_side::sell), 1u);
            SUCCEED();
            return;
        }
        ledger.pending_buy += new_buys;
    }
    FAIL() << "the buy side never closed despite accumulating pending buys";
}

// ── I06 market data goes stale mid-run ──────────────────────────────────────
TEST(MMStrategyIntegration, I06_StaleFeedStopsNewQuotes)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    const auto market = default_market();

    auto ctx = default_context();
    sim_ledger ledger;

    const auto healthy = strat.evaluate(market, ledger.snapshot(), ctx).decision;
    ASSERT_FALSE(healthy.intents.empty());
    ledger.replace_with(healthy);

    // The feed stops: decision time advances, receive time does not.
    for (int i = 1; i <= 10; ++i)
    {
        ctx.decision_time_ns = market.receive_time_ns + i * 100'000'000LL;
        const auto d = strat.evaluate(market, ledger.snapshot(), ctx).decision;
        if (d.market_age_ns > cfg.safety.max_market_data_age_ms * 1'000'000LL)
        {
            EXPECT_EQ(d.state, mm_state::paused) << "step " << i;
            EXPECT_TRUE(d.intents.empty()) << "step " << i;
            EXPECT_TRUE(d.cancel_resting_quotes) << "step " << i;
        }
    }
}

// ── I07 sequence gap then reconciled snapshot ───────────────────────────────
TEST(MMStrategyIntegration, I07_SequenceGapPausesUntilReconciledSnapshot)
{
    auto strat = make_strategy(default_config());
    auto ctx = default_context();
    sim_ledger ledger;

    auto good = default_market();
    const auto before = strat.evaluate(good, ledger.snapshot(), ctx).decision;
    ASSERT_EQ(before.state, mm_state::active);
    ledger.replace_with(before);

    auto gapped = good;
    gapped.sequence_valid = false;
    gapped.snapshot_id = 2;
    for (int i = 0; i < 5; ++i)
    {
        const auto d = strat.evaluate(gapped, ledger.snapshot(), ctx).decision;
        ASSERT_EQ(d.state, mm_state::paused);
        ASSERT_TRUE(d.intents.empty());
        ASSERT_TRUE(d.cancel_resting_quotes);
        ledger.cancel_all();
    }

    auto reconciled = good;
    reconciled.snapshot_id = 7;
    reconciled.event_time_ns = good.event_time_ns + 1'000'000;
    reconciled.receive_time_ns = reconciled.event_time_ns + 100'000;
    ctx.decision_time_ns = reconciled.receive_time_ns + 50'000;

    const auto after = strat.evaluate(reconciled, ledger.snapshot(), ctx).decision;
    EXPECT_EQ(after.state, mm_state::active);
    EXPECT_FALSE(after.intents.empty());
}

// ── I08 replay determinism ──────────────────────────────────────────────────
namespace
{

// One deterministic 500-step replay. Returns the folded decision hash, which
// is the run's result hash.
std::uint64_t run_replay(const mm_config& cfg)
{
    auto strat = make_strategy(cfg);
    auto ctx = default_context();
    sim_ledger ledger;

    std::uint64_t folded = 1469598103934665603ULL;
    std::int64_t bid_ticks = 600000;

    for (int step = 0; step < 500; ++step)
    {
        // Deterministic, seed-free price path: a fixed triangular walk.
        bid_ticks += ((step % 7) - 3);
        auto market = default_market();
        market.best_bid = Price(bid_ticks * ctx.instrument.tick_raw);
        market.best_ask = Price((bid_ticks + 5) * ctx.instrument.tick_raw);
        market.best_bid_qty = atoms_from_decimal("1.00") * (1 + (step % 5));
        market.best_ask_qty = atoms_from_decimal("1.00") * (1 + ((step + 2) % 5));
        market.short_horizon_volatility_bps = 0.1 * static_cast<double>(step % 20);
        market.snapshot_id = static_cast<std::uint64_t>(step + 1);
        market.event_time_ns = base_event_time_ns + step * 1'000'000LL;
        market.receive_time_ns = market.event_time_ns + 100'000;
        ctx.decision_time_ns = market.receive_time_ns + 50'000;

        const auto d = strat.evaluate(market, ledger.snapshot(), ctx).decision;

        folded ^= decision_hash(d);
        folded *= 1099511628211ULL;

        // Deterministic fill rule: every third step fills the level-0 bid,
        // every fifth fills the level-0 ask.
        ledger.replace_with(d);
        if (step % 3 == 0)
        {
            if (const auto* bid = find_intent(d, order_side::buy, 0))
                ledger.position += bid->quantity;
        }
        if (step % 5 == 0)
        {
            if (const auto* ask = find_intent(d, order_side::sell, 0))
                ledger.position -= ask->quantity;
        }
        ledger.cancel_all();
    }
    return folded;
}

} // namespace

TEST(MMStrategyIntegration, I08_FiveIdenticalReplaysProduceOneResultHash)
{
    const auto cfg = default_config();
    const std::uint64_t expected = run_replay(cfg);
    for (int run = 1; run < 5; ++run)
        EXPECT_EQ(run_replay(cfg), expected) << "replay run " << run;

    // A different config must produce a different result hash, otherwise the
    // hash is not actually covering the decision.
    auto other = cfg;
    other.spread.min_half_spread_bps += 1.0;
    EXPECT_NE(run_replay(other), expected);
}

// ── look-ahead ──────────────────────────────────────────────────────────────
TEST(MMStrategyIntegration, LookaheadFutureEventIsRejectedNotConsumed)
{
    auto strat = make_strategy(default_config());
    const auto ctx = default_context();

    auto future_event = default_market();
    future_event.event_time_ns = ctx.decision_time_ns + 1;
    const auto a = strat.evaluate(future_event, flat_inventory(), ctx).decision;
    EXPECT_EQ(a.state, mm_state::paused);
    EXPECT_TRUE(a.intents.empty());
    EXPECT_TRUE(has_reason(a, quote_reason::invalid_market_state));

    auto future_receive = default_market();
    future_receive.receive_time_ns = ctx.decision_time_ns + 1;
    const auto b = strat.evaluate(future_receive, flat_inventory(), ctx).decision;
    EXPECT_EQ(b.state, mm_state::paused);
    EXPECT_TRUE(b.intents.empty());

    // Exactly at the decision instant is visible, not future.
    auto at_boundary = default_market();
    at_boundary.event_time_ns = ctx.decision_time_ns;
    at_boundary.receive_time_ns = ctx.decision_time_ns;
    const auto c = strat.evaluate(at_boundary, flat_inventory(), ctx).decision;
    EXPECT_EQ(c.state, mm_state::active);
}

TEST(MMStrategyIntegration, LookaheadFutureDataCannotAlterAPastDecision)
{
    auto strat = make_strategy(default_config());
    const auto ctx = default_context();

    // The decision that only past data supports.
    const auto baseline = strat.evaluate(default_market(), flat_inventory(), ctx).decision;

    // A later, much better-informed book exists but is stamped after the
    // decision instant. Feeding it must not produce that improved decision.
    auto future = default_market();
    future.event_time_ns = ctx.decision_time_ns + 5'000'000;
    future.receive_time_ns = future.event_time_ns;
    future.best_bid = price_from_decimal("60500.00");
    future.best_ask = price_from_decimal("60500.50");

    const auto with_future = strat.evaluate(future, flat_inventory(), ctx).decision;
    EXPECT_TRUE(with_future.intents.empty());
    EXPECT_EQ(with_future.fair_value.raw(), 0);
    EXPECT_NE(decision_hash(with_future), decision_hash(baseline));
}

// ── hot path: zero allocation after warmup ──────────────────────────────────
namespace
{

class counting_sink final : public IMMDecisionSink
{
public:
    void on_decision(const mm_decision_record& record) noexcept override
    {
        ++calls;
        last_hash = record.decision_hash;
        last_state = record.state;
        last_intents = record.number_of_quote_intents;
        last_config_hash = record.config_hash;
        last_version = record.strategy_version;
        id_size = record.strategy_id.size();
    }

    std::uint64_t calls = 0;
    std::uint64_t last_hash = 0;
    std::uint64_t last_config_hash = 0;
    std::uint32_t last_version = 0;
    mm_state last_state = mm_state::paused;
    std::uint8_t last_intents = 0;
    std::size_t id_size = 0;
};

} // namespace

TEST(MMStrategyIntegration, EvaluateAllocatesNothingAfterWarmup)
{
    auto cfg = default_config();
    cfg.levels = max_quote_levels;
    auto strat = make_strategy(cfg);
    counting_sink sink;
    strat.set_decision_sink(&sink);

    const auto market = default_market();
    const auto ctx = default_context();

    // Warmup covers every branch the measured loop will take.
    std::vector<inventory_snapshot> inventories;
    inventories.push_back(flat_inventory());
    inventories.push_back(inventory_at(cfg.inventory.hard_limit_base / 2));
    inventories.push_back(inventory_at(-cfg.inventory.hard_limit_base / 2));
    inventories.push_back(inventory_at(cfg.inventory.hard_limit_base));
    inventories.push_back(inventory_at(-cfg.inventory.hard_limit_base));

    auto stale_ctx = ctx;
    stale_ctx.decision_time_ns = market.receive_time_ns + 10'000'000'000LL;
    auto gapped = market;
    gapped.sequence_valid = false;

    for (const auto& inv : inventories)
    {
        (void)strat.evaluate(market, inv, ctx);
        (void)strat.evaluate(market, inv, stale_ctx);
        (void)strat.evaluate(gapped, inv, ctx);
    }

    std::uint64_t sink_calls_before = sink.calls;

    truetest::test::alloc::measure_window window;
    for (int i = 0; i < 2000; ++i)
    {
        const auto& inv = inventories[static_cast<std::size_t>(i) % inventories.size()];
        const auto res = strat.evaluate(market, inv, ctx);
        observable_intent_count = res.decision.intents.size();
        (void)strat.evaluate(market, inv, stale_ctx);
        (void)strat.evaluate(gapped, inv, ctx);
    }
    const auto totals = window.total();

    EXPECT_EQ(totals.count, 0u)
        << "evaluate() allocated " << totals.count << " time(s), "
        << totals.bytes << " bytes";
    EXPECT_EQ(sink.calls, sink_calls_before + 6000u);
    EXPECT_GT(sink.last_config_hash, 0u);
    EXPECT_EQ(sink.last_version, InventoryAwareMarketMakingStrategy::version);
    EXPECT_EQ(sink.id_size, cfg.strategy_id.size());
}

// ── telemetry record content ────────────────────────────────────────────────
TEST(MMStrategyIntegration, DecisionSinkReceivesTheFullObservabilityRecord)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    counting_sink sink;
    strat.set_decision_sink(&sink);

    const auto market = default_market();
    const auto ctx = default_context();
    const auto d = strat.evaluate(market, inventory_at(atoms_from_decimal("0.25")), ctx).decision;

    EXPECT_EQ(sink.calls, 1u);
    EXPECT_EQ(sink.last_hash, decision_hash(d));
    EXPECT_EQ(sink.last_state, d.state);
    EXPECT_EQ(static_cast<std::size_t>(sink.last_intents), d.intents.size());
    EXPECT_EQ(sink.last_config_hash, strat.strategy_config_hash());

    // A missing sink must not change the decision.
    auto quiet = make_strategy(cfg);
    const auto quiet_d =
        quiet.evaluate(market, inventory_at(atoms_from_decimal("0.25")), ctx).decision;
    EXPECT_EQ(decision_hash(quiet_d), decision_hash(d));
}
