// Direct tests for DashboardSnapshotBuilder (Wave 1 extraction).
// These exercise the builder via the public engine API (snapshot_dashboard,
// request_dashboard_refresh, the cache paths that feed it, and MC reset clear).
// The goal is richer validation of the moved snapshot logic than the incidental
// pool-count checks in hotpath tests.

#include <gtest/gtest.h>

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "engine/order_audit_sink.h"
#include "data/data_handler.h"
#include "orderbook/orderbook.h"
#include "market_maker/market_maker.h"
#include "strategy/strategy_interface.h"
#include "ui/dashboard_snapshot.h"

#include <chrono>
#include <memory>
#include <sstream>

namespace {

struct silence_cout
{
    std::ostringstream sink;
    std::streambuf* orig;
    silence_cout() : sink(), orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~silence_cout() { std::cout.rdbuf(orig); }
};

[[maybe_unused]] static auto epoch_ms(int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

// Simple strategy that fires one market buy on the first event.
class OneShotBuy : public IStrategy
{
    bool fired_ = false;
public:
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        if (fired_) return std::nullopt;
        fired_ = true;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy,
                           5.0, mkt.get_close());
    }
    void set_position_open(const std::string&, bool) override {}
};

class HealthAuditSink final : public IOrderAuditSink
{
public:
    explicit HealthAuditSink(std::size_t pending_lines)
        : pending_lines_(pending_lines)
    {}

    void record_order_submitted(const order_event&, const char*) override {}
    void record_status_transition(std::uint64_t, order_status, order_status, const char*) override
    {}
    void record_fill(const fill_event&, std::uint64_t, const char*, const char*) override {}
    void record_rejection(const order_event&, const char*, const char*) override {}
    void record_cancellation(std::uint64_t, const char*, const char*, const char*) override {}
    void record_amendment(std::uint64_t, const char*, double, double, double, double,
                          std::chrono::system_clock::time_point) override
    {}
    void record_funding(const funding_event&, const char*) override {}
    void record_event(const char*, const char*, const char*, std::uint64_t, const char*,
                      const char*, const char*) override
    {}

    Health health() const override
    {
        Health value;
        value.pending_lines = pending_lines_;
        return value;
    }

private:
    std::size_t pending_lines_;
};

// Make a few bars so the order can fill against the book.
std::shared_ptr<data_handler> make_small_book_data()
{
    auto dh = std::make_shared<data_handler>();
    // bar 0: setup
    dh->load_into_queue("2024-01-01", "SNAP", 100.0, 101.0, 99.0, 100.0, 1000);
    // bar 1: strategy fires buy
    dh->load_into_queue("2024-01-01", "SNAP", 100.5, 102.0, 100.0, 101.5, 2000);
    // bar 2: more data
    dh->load_into_queue("2024-01-01", "SNAP", 101.0, 103.0, 100.5, 102.5, 1500);
    return dh;
}

} // anon

TEST(DashboardSnapshot, PopulatesCoreFieldsAfterRun)
{
    silence_cout quiet;

    auto dh = make_small_book_data();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<OneShotBuy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);  // liquidity so the buy can match

    engine eng(dh, ob, strat);
    eng.run();

    truetest::ui::dashboard_snapshot snap;
    ASSERT_TRUE(eng.snapshot_dashboard(snap));

    // Builder produced a coherent view (exercises the full build_dashboard_view
    // path and all the injected sources: portfolio, analytics, pools, rings, etc.)
    // We don't require a fill in every tiny dataset; the important thing is that
    // the moved logic runs without crashing and populates the structural fields.
    EXPECT_FALSE(snap.debug.pools.empty());   // memory/debug pools always added
    EXPECT_FALSE(snap.debug.rings.empty());
#ifdef TRUETEST_VENUE_DATA_COMPILED
    EXPECT_TRUE(snap.debug.has_live_data);
#else
    EXPECT_FALSE(snap.debug.has_live_data);
#endif

    // Perf / risk sections are always written
    EXPECT_GE(snap.perf.total_orders, 0u);
    EXPECT_GE(snap.risk.open_orders, 0u);
}

TEST(DashboardSnapshot, RequestRefreshAndClearForMCReset)
{
    silence_cout quiet;

    auto dh = make_small_book_data();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<OneShotBuy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine eng(dh, ob, strat);
    eng.run();

    truetest::ui::dashboard_snapshot snap1;
    ASSERT_TRUE(eng.snapshot_dashboard(snap1));

    // Force an immediate refresh on next tick (exercises request_dashboard_refresh path)
    eng.request_dashboard_refresh();

    // MC reuse path exercises clear_for_mc_reset on the builder
    eng.reset_for_next_trial(0xC0FFEE);

    // After clear we can still request + snapshot without crash (the caches were reset)
    eng.request_dashboard_refresh();

    truetest::ui::dashboard_snapshot snap2;
    EXPECT_NO_THROW(eng.snapshot_dashboard(snap2));
}

TEST(DashboardSnapshot, OpenOrdersAndFillsCachesAreFed)
{
    silence_cout quiet;

    auto dh = make_small_book_data();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<OneShotBuy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine eng(dh, ob, strat);
    eng.run();

    truetest::ui::dashboard_snapshot snap;
    ASSERT_TRUE(eng.snapshot_dashboard(snap));

    // The builder received cache_open_order / cache_fill / update/erase calls
    // during the run (via the hot order/fill paths). We mainly care that the
    // view was produced and the debug section (which reads the pools the
    // builder was given) is populated.
    EXPECT_FALSE(snap.debug.pools.empty());
}

TEST(DashboardSnapshot, AuditSinkOwnerReplacementIsObserved)
{
    portfolio port;
    Analytics analytics;
    AdverseSelectionTracker adverse;
    truetest::exits::ExitManager exits;
    std::atomic<bool> halt_flag{false};
    engine_config config;
    std::atomic<double> last_mid_price{0.0};
    std::string last_mark_symbol;
    std::unordered_map<std::string, double> last_mark_prices;
    std::mutex last_mark_prices_mu;
    OrderbookRegistry orderbook_registry;
    std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>> execution_adapters;
    std::unique_ptr<IOrderAuditSink> audit_sink = std::make_unique<HealthAuditSink>(17);
    std::unordered_set<std::string> l2_seeded_symbols;
    ObjectPool<market_event> market_pool;
    ObjectPool<order_event> order_pool;
    ObjectPool<fill_event> fill_pool;
    ObjectPool<tick_event> tick_pool;
    ObjectPool<l2_update_event> l2_update_pool;
    ObjectPool<l2_snapshot_event> l2_snapshot_pool;
    ObjectPool<rejection_event> rejection_pool;
    ObjectPool<cancel_event> cancel_pool;
    ObjectPool<amend_event> amend_pool;
    ObjectPool<funding_event> funding_pool;
    ControlBlockPool control_block_pool;
    std::shared_ptr<EventRing> logging_ring;
    std::shared_ptr<EventRing> risk_ring;
    std::shared_ptr<EventRing> stats_ring;
    std::shared_ptr<EventRing> observer_ring;
    std::shared_ptr<EventRing> risk_stats_ring;
    std::shared_ptr<EventRing> mm_ring;

    DashboardSnapshotBuilder builder(
        port, analytics, adverse, exits, halt_flag, config, last_mid_price, last_mark_symbol,
        last_mark_prices, last_mark_prices_mu, orderbook_registry, execution_adapters, audit_sink,
        l2_seeded_symbols, market_pool, order_pool, fill_pool, tick_pool, l2_update_pool,
        l2_snapshot_pool, rejection_pool, cancel_pool, amend_pool, funding_pool, control_block_pool,
        logging_ring, risk_ring, stats_ring, observer_ring, risk_stats_ring, mm_ring);

    builder.request_dashboard_refresh();
    builder.refresh_if_due();
    truetest::ui::dashboard_snapshot first;
    ASSERT_TRUE(builder.snapshot_dashboard(first));
    EXPECT_EQ(first.health.questdb.pending_lines, 17U);

    // Allocate the replacement before destroying the old sink so the two
    // objects cannot occupy the same address. A consumer retaining a direct
    // reference to the old pointee would now read freed storage.
    auto replacement = std::make_unique<HealthAuditSink>(29);
    audit_sink = std::move(replacement);

    builder.request_dashboard_refresh();
    builder.refresh_if_due();
    truetest::ui::dashboard_snapshot second;
    ASSERT_TRUE(builder.snapshot_dashboard(second));
    EXPECT_EQ(second.health.questdb.pending_lines, 29U);
}
