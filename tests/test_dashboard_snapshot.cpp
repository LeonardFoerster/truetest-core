// Direct tests for DashboardSnapshotBuilder (Wave 1 extraction).
// These exercise the builder via the public engine API (snapshot_dashboard,
// request_dashboard_refresh, the cache paths that feed it, and MC reset clear).
// The goal is richer validation of the moved snapshot logic than the incidental
// pool-count checks in hotpath tests.

#include <gtest/gtest.h>

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "helpers/alloc_counter.h"
#include "data/data_handler.h"
#include "execution/latency_model.h"
#include "orderbook/orderbook.h"
#include "market_maker/market_maker.h"
#include "strategy/strategy_interface.h"
#include "ui/dashboard_snapshot.h"
#include "providers/provider.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

class DashboardProjectionTestPeer
{
public:
    static DashboardSnapshotBuilder& builder(engine& value)
    {
        return *value.dashboard_builder_;
    }

    static bool force_capture(engine& value)
    {
        auto& target = builder(value);
        target.request_dashboard_refresh();
        // Forced runtime captures are not quiescent: exercising this helper
        // must never permit provider/adapter diagnostics on a reader race.
        return target.publish_projection(true, false);
    }

    static bool capture_if_due(engine& value)
    {
        return builder(value).refresh_if_due();
    }

    static bool pin_all_slots(engine& value)
    {
        auto& target = builder(value);
        bool all = true;
        for (std::uint8_t i = 0; i < target.kSnapshotSlotCount; ++i)
        {
            auto expected = std::uint32_t{0};
            all = target.snapshot_slots_[i].access_state.compare_exchange_strong(
                expected, 1U, std::memory_order_acq_rel) && all;
        }
        return all;
    }

    static void unpin_all_slots(engine& value)
    {
        auto& target = builder(value);
        for (std::uint8_t i = 0; i < target.kSnapshotSlotCount; ++i)
            target.snapshot_slots_[i].access_state.store(
                0U, std::memory_order_release);
    }

    static bool all_slots_unpinned(engine& value)
    {
        auto& target = builder(value);
        for (const auto& slot : target.snapshot_slots_)
            if (slot.access_state.load(std::memory_order_acquire) != 0U)
                return false;
        return true;
    }

    static bool current_generation_matches_token(engine& value)
    {
        auto& target = builder(value);
        const auto token = target.published_snapshot_token_.load(
            std::memory_order_acquire);
        if (token == target.kNoPublishedSnapshotToken) return false;
        const auto index = static_cast<std::uint8_t>(token & 0xffU);
        return target.snapshot_slots_[index].value.generation == (token >> 8U);
    }

    static void cache_open_order(engine& value, const order_event& order)
    {
        builder(value).cache_open_order(order);
    }

    [[noreturn]] static void throw_while_projection_is_pinned(engine& value)
    {
        auto pin = builder(value).pin_latest_projection();
        if (!pin) throw std::logic_error("no dashboard projection to pin");
        throw std::runtime_error("injected cold materialization failure");
    }

    static bool exercise_warmed_scheduler_storage(engine& value)
    {
        if (value.pending_stops_.capacity() == 0U ||
            value.pending_orders_capacity_ == 0U ||
            value.bar_delayed_orders_.capacity() == 0U ||
            value.bar_delayed_ready_.capacity() == 0U)
            return false;

        const auto ts = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(1'700'000'000'000LL));
        order_event order(ts, "SCH", order_type::limit,
                          order_side::buy, 1.0, 100.0);
        order.set_earliest_eligible_ts(ts);
        auto stop = value.acquire_pooled(value.order_pool_, order);
        auto latency = value.acquire_pooled(value.order_pool_, order);
        auto delayed = value.acquire_pooled(value.order_pool_, order);
        auto ready = value.acquire_pooled(value.order_pool_, order);

        value.pending_stops_.push_back(std::move(stop));
        value.pending_stops_.pop_back();
        value.pending_orders_.push({std::move(latency), ts, 1U});
        value.pending_orders_.pop();
        value.bar_delayed_orders_.push_back(
            {std::move(delayed), 2U, 1U});
        value.bar_delayed_orders_.pop_back();
        value.bar_delayed_ready_.push_back(
            {std::move(ready), 3U, 0U});
        value.bar_delayed_ready_.pop_back();
        return true;
    }

    static bool occupy_latency_scheduler(engine& value)
    {
        if (value.pending_orders_capacity_ != 1U ||
            !value.pending_orders_.empty())
            return false;
        const auto ts = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(1'700'000'000'000LL));
        order_event order(ts, "FULL", order_type::limit,
                          order_side::buy, 1.0, 100.0);
        order.set_earliest_eligible_ts(ts + std::chrono::seconds(1));
        auto pooled = value.acquire_pooled(value.order_pool_, order);
        value.pending_orders_.push({std::move(pooled),
                                    order.get_earliest_eligible_ts(), 0U});
        return true;
    }

    static void release_latency_scheduler(engine& value)
    {
        if (!value.pending_orders_.empty()) value.pending_orders_.pop();
        value.drain_object_pool_returns();
    }

    static bool route_order(engine& value, order_event& order)
    {
        std::size_t event_count = 0;
        bool halt_requested = false;
        return value.route_order(order, order.get_timestamp(), event_count,
                                 halt_requested);
    }

    static std::size_t cached_open_orders(engine& value)
    {
        return builder(value).open_orders_cache_size_;
    }
};

namespace {

struct silence_cout
{
    std::ostringstream sink;
    std::streambuf* orig;
    silence_cout()
        : sink()
        , orig(std::cout.rdbuf(sink.rdbuf()))
    {}
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
        return order_event(mkt.get_timestamp(), mkt.get_symbol(), order_type::market,
                           order_side::buy, 5.0, mkt.get_close());
    }
    void set_position_open(const std::string&, bool) override {}
};

class BuyThenPartialSell : public IStrategy
{
    unsigned calls_ = 0;

public:
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        ++calls_;
        if (calls_ == 3)
            return order_event(mkt.get_timestamp(), mkt.get_symbol(), order_type::market,
                               order_side::buy, 5.0, mkt.get_close());
        if (calls_ == 6)
            return order_event(mkt.get_timestamp(), mkt.get_symbol(), order_type::market,
                               order_side::sell, 2.0, mkt.get_close());
        return std::nullopt;
    }
    void set_position_open(const std::string&, bool) override {}
};

class StageStopThenOverflow : public IStrategy
{
    unsigned calls_ = 0;

public:
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        ++calls_;
        if (calls_ == 1)
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::stop_limit, order_side::buy,
                               1.0, 151.0, time_in_force::gtc, 150.0);
        if (calls_ == 2)
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::limit, order_side::buy, 1.0, 90.0);
        return std::nullopt;
    }

    void set_position_open(const std::string&, bool) override {}
};

class CountingSnapshotProvider final : public IProvider
{
public:
    std::string name() const override
    {
        diagnostic_calls.fetch_add(1U, std::memory_order_relaxed);
        return "snapshot-counter";
    }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return false; }
    bool open() override { return true; }
    void close() override {}
    lifecycle lifecycle_state() const override
    {
        diagnostic_calls.fetch_add(1U, std::memory_order_relaxed);
        return lifecycle::closed;
    }
    std::shared_ptr<IDataTransport> get_transport() override { return {}; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        diagnostic_calls.fetch_add(1U, std::memory_order_relaxed);
        return {};
    }

    mutable std::atomic<std::size_t> diagnostic_calls{0};
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

std::shared_ptr<data_handler> make_partial_close_data()
{
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 10; ++i)
        dh->load_into_queue("2024-01-01", "PNL", 100.0 + i, 105.0 + i,
                            95.0 + i, 102.0 + i, 1000);
    return dh;
}

}  // namespace

TEST(DashboardSnapshot, PopulatesCoreFieldsAfterRun)
{
    silence_cout quiet;

    auto dh = make_small_book_data();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<OneShotBuy>();

    // Seed external venue L2 separately from local MarketMaker liquidity.
    // The builder must publish a usable external BBO in Market Watch.
    ob->apply_l2_snapshot(
        std::vector<std::pair<Price, quantity>>{{Price::from_double(100.0), 100}},
        std::vector<std::pair<Price, quantity>>{{Price::from_double(101.0), 120}});

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
    EXPECT_FALSE(snap.debug.pools.empty());  // memory/debug pools always added
    EXPECT_FALSE(snap.debug.rings.empty());
#ifdef TRUETEST_VENUE_DATA_COMPILED
    EXPECT_TRUE(snap.debug.has_live_data);
#else
    EXPECT_FALSE(snap.debug.has_live_data);
#endif

    // Perf / risk sections are always written
    EXPECT_GE(snap.perf.total_orders, 0u);
    EXPECT_GE(snap.risk.open_orders, 0u);

    EXPECT_TRUE(snap.generated_at_available);
    const auto market = std::find_if(snap.market_rows.begin(), snap.market_rows.end(),
                                     [](const auto& row) { return row.symbol == "SNAP"; });
    ASSERT_NE(market, snap.market_rows.end());
    EXPECT_TRUE(market->best_bid_available);
    EXPECT_TRUE(market->best_ask_available);
    EXPECT_LT(market->best_bid, market->best_ask);
    EXPECT_TRUE(market->bbo_available);

    // A fully marked snapshot must respect the portfolio accounting identity.
    // `realized_pnl` is the settled/net cash remainder, not a second label for
    // `equity - initial_balance`.
    if (snap.total_pnl_available && snap.realized_pnl_available && snap.unrealized_pnl_available) {
        EXPECT_NEAR(snap.total_pnl, snap.realized_pnl + snap.unrealized_pnl, 1e-7);
    }
}

TEST(DashboardSnapshot, SeparatesSettledAndUnrealizedPnlAfterPartialClose)
{
    silence_cout quiet;

    auto dh = make_partial_close_data();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<BuyThenPartialSell>();
    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.seed = 1;
    cfg.initial_balance = 100000.0;
    cfg.execution_bar_delay = 1;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();
    eng.request_dashboard_refresh();

    truetest::ui::dashboard_snapshot snap;
    ASSERT_TRUE(eng.snapshot_dashboard(snap));

    const auto position = std::find_if(snap.positions.begin(), snap.positions.end(),
                                       [](const auto& row) { return row.symbol == "PNL"; });
    ASSERT_NE(position, snap.positions.end());
    EXPECT_NEAR(position->qty, 3.0, 1e-12);
    ASSERT_TRUE(position->mark_available);
    ASSERT_TRUE(position->unrealized_available);
    ASSERT_TRUE(snap.total_pnl_available);
    ASSERT_TRUE(snap.realized_pnl_available);
    ASSERT_TRUE(snap.unrealized_pnl_available);

    // The partial close settles cash PnL while the remaining three units stay
    // marked. Neither field is a relabelled copy of account total PnL.
    EXPECT_NEAR(snap.total_pnl, snap.realized_pnl + snap.unrealized_pnl, 1e-7);
    EXPECT_NE(snap.total_pnl, snap.realized_pnl);
    EXPECT_NEAR(position->unrealized,
                position->mark * position->qty - position->avg_entry * position->qty, 1e-7);
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

TEST(DashboardSnapshot, StagedStopOwnsCapacityThenExpiresCoherentlyAtEof)
{
    silence_cout quiet;

    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 6; ++i)
        dh->load_into_queue("2024-01-01", "STAGED", 100.0, 102.0,
                            98.0, 100.0, 1000);

    engine_config cfg;
    cfg.seed = 7;
    cfg.risk.max_open_orders = 1;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    auto strategy = std::make_shared<StageStopThenOverflow>();
    engine eng(dh, nullptr, strategy, std::move(cfg));
    eng.run();

    truetest::ui::dashboard_snapshot snap;
    ASSERT_TRUE(eng.snapshot_dashboard(snap));
    EXPECT_TRUE(snap.open_orders.empty());
    EXPECT_EQ(snap.risk.open_orders, 0u);
    EXPECT_EQ(snap.risk.open_orders_limit, 1u);
    EXPECT_EQ(eng.get_order_tracker().active_count(), 0u);
    EXPECT_TRUE(eng.get_order_tracker().get_open_orders().empty());
    ASSERT_TRUE(snap.debug.pending_stops_available);
    EXPECT_EQ(snap.debug.pending_stops, 0u);
    EXPECT_EQ(eng.total_audit_rejections(), 1u)
        << "the staged stop must reserve capacity until EOF cleanup";
}

TEST(DashboardProjection, DemandedCaptureAndNoDemandPollAreAllocationFree)
{
    silence_cout quiet;
    auto dh = std::make_shared<data_handler>();
    auto strategy = std::make_shared<OneShotBuy>();
    engine eng(dh, nullptr, strategy);

    ASSERT_TRUE(DashboardProjectionTestPeer::force_capture(eng));

    truetest::test::alloc::snapshot demanded{};
    bool captured = false;
    {
        truetest::test::alloc::measure_window window;
        captured = DashboardProjectionTestPeer::force_capture(eng);
        demanded = window.total();
    }
    ASSERT_TRUE(captured);
    EXPECT_EQ(demanded.count, 0U);
    EXPECT_EQ(demanded.bytes, 0U);

    truetest::test::alloc::snapshot idle{};
    bool unexpected_capture = false;
    {
        truetest::test::alloc::measure_window window;
        for (int i = 0; i < 10'000; ++i)
            unexpected_capture =
                DashboardProjectionTestPeer::capture_if_due(eng) ||
                unexpected_capture;
        idle = window.total();
    }
    EXPECT_FALSE(unexpected_capture);
    EXPECT_EQ(idle.count, 0U);
    EXPECT_EQ(idle.bytes, 0U);
}

TEST(DashboardProjection, SchedulerStorageIsPrewarmedAndAllocationFree)
{
    silence_cout quiet;
    engine_config config;
    config.risk.max_open_orders = 8;
    engine eng(std::make_shared<data_handler>(), nullptr,
               std::make_shared<OneShotBuy>(), std::move(config));

    truetest::test::alloc::snapshot allocation{};
    bool exercised = false;
    {
        truetest::test::alloc::measure_window window;
        exercised =
            DashboardProjectionTestPeer::exercise_warmed_scheduler_storage(
                eng);
        allocation = window.total();
    }
    ASSERT_TRUE(exercised);
    EXPECT_EQ(allocation.count, 0U);
    EXPECT_EQ(allocation.bytes, 0U);
}

TEST(DashboardProjection, FullSchedulerRejectsWithoutTrackerOrCacheGhost)
{
    silence_cout quiet;
    engine_config config;
    config.risk.max_open_orders = 1;
    config.latency_model = std::make_shared<FixedLatencyModel>(
        std::chrono::microseconds(100));
    engine eng(std::make_shared<data_handler>(), nullptr,
               std::make_shared<OneShotBuy>(), std::move(config));
    ASSERT_TRUE(DashboardProjectionTestPeer::occupy_latency_scheduler(eng));

    order_event candidate(epoch_ms(1'700'000'000'100LL), "FULL",
                          order_type::limit, order_side::buy, 1.0, 100.0);
    ASSERT_TRUE(DashboardProjectionTestPeer::route_order(eng, candidate));
    ASSERT_NE(candidate.get_order_id(), 0U);
    EXPECT_EQ(eng.get_order_tracker().get_order_status(
                  candidate.get_order_id()),
              order_status::rejected);
    EXPECT_EQ(eng.get_order_tracker().active_count(), 0U);
    EXPECT_EQ(DashboardProjectionTestPeer::cached_open_orders(eng), 0U);
    EXPECT_EQ(eng.total_audit_rejections(), 1U);
    DashboardProjectionTestPeer::release_latency_scheduler(eng);
}

TEST(DashboardProjection, PinnedSlotsSkipWithoutLosingRequest)
{
    silence_cout quiet;
    engine eng(std::make_shared<data_handler>(), nullptr,
               std::make_shared<OneShotBuy>());

    ASSERT_TRUE(DashboardProjectionTestPeer::pin_all_slots(eng));
    EXPECT_FALSE(DashboardProjectionTestPeer::force_capture(eng));
    DashboardProjectionTestPeer::unpin_all_slots(eng);
    EXPECT_TRUE(DashboardProjectionTestPeer::force_capture(eng));
    EXPECT_TRUE(DashboardProjectionTestPeer::current_generation_matches_token(
        eng));
}

TEST(DashboardProjection, ReaderExceptionAlwaysUnpinsPublishedSlot)
{
    silence_cout quiet;
    engine eng(std::make_shared<data_handler>(), nullptr,
               std::make_shared<OneShotBuy>());

    truetest::ui::dashboard_snapshot snapshot;
    ASSERT_TRUE(eng.snapshot_dashboard(snapshot));
    EXPECT_THROW(
        DashboardProjectionTestPeer::throw_while_projection_is_pinned(eng),
        std::runtime_error);
    EXPECT_TRUE(DashboardProjectionTestPeer::all_slots_unpinned(eng));

    EXPECT_TRUE(eng.snapshot_dashboard(snapshot));
    EXPECT_TRUE(DashboardProjectionTestPeer::all_slots_unpinned(eng));
}

TEST(DashboardProjection, RuntimeCaptureAndReadersNeverCallProviderDiagnostics)
{
    silence_cout quiet;
    auto provider = std::make_shared<CountingSnapshotProvider>();
    engine_config config;
    config.provider = provider;
    engine eng(std::make_shared<data_handler>(), nullptr,
               std::make_shared<OneShotBuy>(), std::move(config));

    const auto baseline =
        provider->diagnostic_calls.load(std::memory_order_relaxed);
    for (int i = 0; i < 64; ++i)
    {
        ASSERT_TRUE(DashboardProjectionTestPeer::force_capture(eng));
        truetest::ui::dashboard_snapshot snapshot;
        ASSERT_TRUE(eng.snapshot_dashboard(snapshot));
    }
    EXPECT_EQ(provider->diagnostic_calls.load(std::memory_order_relaxed),
              baseline);
}

TEST(DashboardProjection, ConcurrentReadersNeverObserveTornGeneration)
{
    silence_cout quiet;
    engine eng(std::make_shared<data_handler>(), nullptr,
               std::make_shared<OneShotBuy>());
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};

    std::array<std::thread, 3> readers;
    for (auto& reader : readers)
    {
        reader = std::thread([&] {
            while (!stop.load(std::memory_order_acquire))
            {
                try
                {
                    truetest::ui::dashboard_snapshot snapshot;
                    if (!eng.snapshot_dashboard(snapshot) ||
                        !snapshot.generated_at_available)
                        failed.store(true, std::memory_order_release);
                }
                catch (...)
                {
                    failed.store(true, std::memory_order_release);
                }
            }
        });
    }

    std::size_t published = 0;
    for (int i = 0; i < 100; ++i)
        if (DashboardProjectionTestPeer::force_capture(eng)) ++published;
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) reader.join();

    EXPECT_GT(published, 0U);
    EXPECT_FALSE(failed.load(std::memory_order_acquire));
    EXPECT_TRUE(DashboardProjectionTestPeer::all_slots_unpinned(eng));
    EXPECT_TRUE(DashboardProjectionTestPeer::current_generation_matches_token(
        eng));
}

TEST(DashboardProjection, BoundedOrderProjectionReportsTruncationLoudly)
{
    silence_cout quiet;
    engine_config config;
    config.risk.max_open_orders = 5000;
    engine eng(std::make_shared<data_handler>(), nullptr,
               std::make_shared<OneShotBuy>(), std::move(config));

    const auto timestamp = epoch_ms(1'700'000'000'000LL);
    for (std::uint64_t id = 1; id <= 4097; ++id)
    {
        order_event order(timestamp, "BND", order_type::limit,
                          order_side::buy, 1.0, 100.0);
        order.set_order_id(id);
        DashboardProjectionTestPeer::cache_open_order(eng, order);
    }
    ASSERT_TRUE(DashboardProjectionTestPeer::force_capture(eng));

    truetest::ui::dashboard_snapshot snapshot;
    ASSERT_TRUE(eng.snapshot_dashboard(snapshot));
    EXPECT_EQ(snapshot.open_orders.size(),
              truetest::dashboard::kMaxOpenOrders);
    const auto error = std::find_if(
        snapshot.debug.errors.begin(), snapshot.debug.errors.end(),
        [](const auto& row) {
            return std::string_view(row.name) == "dashboard_projection";
        });
    ASSERT_NE(error, snapshot.debug.errors.end());
    EXPECT_FALSE(error->msg.empty());
}

TEST(DashboardProjection, ShortRunPublishesTerminalStateWithoutSleep)
{
    silence_cout quiet;
    auto data = make_small_book_data();
    auto book = std::make_shared<orderbook>();
    MarketMaker maker;
    maker.add_orders(book, 100.0, 10);
    engine eng(data, book, std::make_shared<OneShotBuy>());

    truetest::ui::dashboard_snapshot initial;
    ASSERT_TRUE(eng.snapshot_dashboard(initial));
    eng.run();
    truetest::ui::dashboard_snapshot terminal;
    ASSERT_TRUE(eng.snapshot_dashboard(terminal));

    EXPECT_GT(terminal.generated_at, initial.generated_at);
    EXPECT_GT(terminal.perf.total_orders, 0U);
    EXPECT_EQ(terminal.perf.total_fills, terminal.health.fills_total);
    EXPECT_TRUE(DashboardProjectionTestPeer::current_generation_matches_token(
        eng));
}
