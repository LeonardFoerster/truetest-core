#include <gtest/gtest.h>
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "orderbook/orderbook.h"
#include "execution/latency_model.h"
#include "market_maker/market_maker.h"
#include <sstream>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>

// RAII helper to silence cout during noisy backtest runs.
// Anonymous namespace: same struct name exists in other test TUs; without
// this scope, UBSAN/LTO can pick the wrong definition across translation
// units and fault during destruction.
namespace {
struct SilenceCout {
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceCout() : sink(), orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~SilenceCout() { std::cout.rdbuf(orig); }
};
}

static auto epoch_ms(int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

// Test strategy: buys on bar 3, sells on bar 6
class TestStrategy : public IStrategy
{
    bool position_open_ = false;
    int call_count_ = 0;
public:
    int get_call_count() const { return call_count_; }

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        call_count_++;
        if (call_count_ == 3 && !position_open_)
        {
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::buy, 10, mkt.get_close());
        }
        if (call_count_ == 6 && position_open_)
        {
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::sell, 10, mkt.get_close());
        }
        return std::nullopt;
    }
    void set_position_open(const std::string&, bool open) override { position_open_ = open; }
};

class CoordinatedThrowStrategy final : public IStrategy
{
public:
    std::optional<order_event> on_market(const market_event&) override
    {
        std::unique_lock<std::mutex> lock(mu_);
        entered_ = true;
        entered_cv_.notify_one();
        release_cv_.wait(lock, [this] { return release_; });
        ++calls_;
        throw std::runtime_error("coordinated strategy failure");
    }

    void set_position_open(const std::string&, bool) override {}

    void wait_until_entered()
    {
        std::unique_lock<std::mutex> lock(mu_);
        entered_cv_.wait(lock, [this] { return entered_; });
    }

    void release_failure()
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            release_ = true;
        }
        release_cv_.notify_one();
    }

    int calls() const noexcept { return calls_.load(std::memory_order_acquire); }

private:
    std::mutex mu_;
    std::condition_variable entered_cv_;
    std::condition_variable release_cv_;
    bool entered_ = false;
    bool release_ = false;
    std::atomic<int> calls_{0};
};

class ThrowOnCleanupProvider final : public IProvider
{
public:
    std::string name() const override { return "throw-on-cleanup"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return false; }
    bool open() override { return true; }
    void close() override {}
    std::shared_ptr<IDataTransport> get_transport() override { return {}; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        if (adapter_queries_++ > 0)
            throw std::runtime_error("injected cleanup callback failure");
        return {};
    }

private:
    int adapter_queries_ = 0;
};

static std::shared_ptr<data_handler> make_bar_data(int n)
{
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < n; ++i)
        dh->load_into_queue("2024-01-01", "TEST",
                            100.0 + i, 105.0 + i, 95.0 + i, 102.0 + i, 1000);
    return dh;
}

static std::shared_ptr<data_handler> make_tick_data(int n)
{
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < n; ++i)
    {
        tick_record t;
        t.timestamp = epoch_ms(i * 100);
        t.symbol = "TEST";
        t.price = 100.0 + (i % 10) * 0.1;
        t.quantity = 10;
        t.side = data_tick_side::unknown;
        dh->add_tick(std::move(t));
    }
    return dh;
}

TEST(Engine, EmptyData_Throws)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<TestStrategy>();
    engine eng(dh, ob, strat);
    EXPECT_THROW(eng.run(), std::runtime_error);
}

TEST(Engine, DestructorContainsProviderCleanupExceptions)
{
    SilenceCout quiet;
    engine_config cfg;
    cfg.provider = std::make_shared<ThrowOnCleanupProvider>();

    EXPECT_NO_THROW({
        engine eng(std::make_shared<data_handler>(),
                   std::make_shared<orderbook>(),
                   std::make_shared<TestStrategy>(), std::move(cfg));
    });
}

TEST(Engine, RunCompletes)
{
    SilenceCout quiet;
    auto dh = make_bar_data(10);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<TestStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine eng(dh, ob, strat);
    EXPECT_NO_THROW(eng.run());
}

TEST(Engine, PreexistingTerminalHaltIsNotClearedByRunStartup)
{
    SilenceCout quiet;
    auto dh = make_bar_data(5);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<TestStrategy>();
    engine eng(dh, ob, strat);

    eng.trigger_halt("test pre-start halt");
    eng.run();

    EXPECT_TRUE(eng.is_halted());
    EXPECT_FALSE(eng.run_succeeded());
    EXPECT_EQ(strat->get_call_count(), 0);
}

TEST(Engine, DirectTerminalHaltFlagCannotReportSuccess)
{
    auto dh = make_bar_data(1);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<TestStrategy>();
    engine eng(dh, ob, strat);

    eng.trigger_halt("test pre-halt");

    EXPECT_FALSE(eng.run_succeeded());
}

TEST(Engine, StrategyReceivesEvents)
{
    SilenceCout quiet;
    auto dh = make_bar_data(10);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<TestStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine eng(dh, ob, strat);
    eng.run();
    EXPECT_EQ(strat->get_call_count(), 10);
}

TEST(Engine, TickData_DispatchesToRunTickData)
{
    SilenceCout quiet;
    auto dh = make_tick_data(100);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<TestStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine eng(dh, ob, strat);
    EXPECT_NO_THROW(eng.run());
}

TEST(Engine, LatencyModel_DelaysOrders)
{
    SilenceCout quiet;
    auto dh = make_bar_data(10);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<TestStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.latency_model = std::make_shared<FixedLatencyModel>(latency_duration(500));

    engine eng(dh, ob, strat, std::move(cfg));
    EXPECT_NO_THROW(eng.run());
}

TEST(Engine, ThreadingDisabled_NoRings)
{
    SilenceCout quiet;
    auto dh = make_bar_data(5);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<TestStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine eng(dh, ob, strat);
    eng.run();
    EXPECT_EQ(eng.get_logging_ring(), nullptr);
    EXPECT_EQ(eng.get_risk_ring(), nullptr);
    EXPECT_EQ(eng.get_stats_ring(), nullptr);
    EXPECT_EQ(eng.get_observer_ring(), nullptr);
    EXPECT_EQ(eng.get_risk_stats_ring(), nullptr);
}

TEST(Engine, FullPreset_RingsCreated)
{
    SilenceCout quiet;
    auto dh = make_bar_data(5);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<TestStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.threading = thread_preset::full;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();
    EXPECT_NE(eng.get_logging_ring(), nullptr);
    EXPECT_NE(eng.get_risk_ring(), nullptr);
    EXPECT_NE(eng.get_stats_ring(), nullptr);
}

TEST(Engine, LightPreset_ObserverWorkerRuns)
{
    SilenceCout quiet;
    auto dh = make_bar_data(50);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<TestStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.threading = thread_preset::light;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    EXPECT_NE(eng.get_observer_ring(), nullptr);
    EXPECT_NE(eng.get_observer_worker(), nullptr);
    EXPECT_GT(eng.get_observer_worker()->events_processed(), 0u);
    EXPECT_TRUE(eng.get_observer_ring()->empty());
}

TEST(Engine, StandardPreset_LoggingAndRiskStatsRun)
{
    SilenceCout quiet;
    auto dh = make_bar_data(50);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<TestStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.threading = thread_preset::standard;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    EXPECT_NE(eng.get_logging_ring(), nullptr);
    EXPECT_NE(eng.get_risk_stats_ring(), nullptr);
    EXPECT_NE(eng.get_logging_worker(), nullptr);
    EXPECT_NE(eng.get_risk_stats_worker(), nullptr);
    EXPECT_GT(eng.get_logging_worker()->events_processed(), 0u);
    EXPECT_GT(eng.get_risk_stats_worker()->events_processed(), 0u);
    EXPECT_TRUE(eng.get_logging_ring()->empty());
    EXPECT_TRUE(eng.get_risk_stats_ring()->empty());
}

// --- Step 9: Threading worker tests ---

TEST(Engine, ThreadingEnabled_WorkersRun)
{
    SilenceCout quiet;
    auto dh = make_bar_data(50);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<TestStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.threading = thread_preset::full;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    // Workers should have processed events (at least market events were published)
    EXPECT_GT(eng.get_logging_worker()->events_processed(), 0u);
    EXPECT_GT(eng.get_stats_worker()->events_processed(), 0u);
    // Risk worker also receives all published events
    EXPECT_GT(eng.get_risk_worker()->events_processed(), 0u);
}

TEST(Engine, HaltChannel_StopsEngine)
{
    SilenceCout quiet;
    auto dh = make_bar_data(100000);
    auto ob = std::make_shared<orderbook>();

    // Strategy that buys on bar 3 and never sells - we'll inject halt externally
    auto strat = std::make_shared<TestStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.threading = thread_preset::full;

    engine eng(dh, ob, strat, std::move(cfg));

    // Inject halt after a short delay from another thread
    std::thread injector([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        eng.trigger_halt("test requested halt");
    });

    eng.run();
    injector.join();

    // Engine should have stopped early (strategy didn't see all 100000 bars)
    EXPECT_LT(strat->get_call_count(), 100000);
}

TEST(Engine, GracefulShutdown_RingsDrained)
{
    SilenceCout quiet;
    auto dh = make_bar_data(20);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<TestStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.threading = thread_preset::full;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    // After run() completes, all rings should be fully drained
    EXPECT_TRUE(eng.get_logging_ring()->empty());
    EXPECT_TRUE(eng.get_risk_ring()->empty());
    EXPECT_TRUE(eng.get_stats_ring()->empty());
}

TEST(Engine, StrategyExceptionStopsWorkersAndTerminallyHalts)
{
    SilenceCout quiet;
    auto dh = make_bar_data(2);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<CoordinatedThrowStrategy>();

    engine_config cfg;
    cfg.threading = thread_preset::light;
    engine eng(dh, ob, strat, std::move(cfg));

    std::exception_ptr failure;
    std::thread runner([&] {
        try { eng.run(); }
        catch (...) { failure = std::current_exception(); }
    });

    strat->wait_until_entered();
    auto* worker = eng.get_observer_worker();
    const auto worker_start_deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (worker && !worker->is_running() &&
           std::chrono::steady_clock::now() < worker_start_deadline)
        std::this_thread::yield();
    const bool worker_was_running = worker && worker->is_running();

    strat->release_failure();
    runner.join();

    ASSERT_NE(failure, nullptr);
    EXPECT_THROW(std::rethrow_exception(failure), std::runtime_error);
    EXPECT_TRUE(worker_was_running);
    ASSERT_NE(worker, nullptr);
    EXPECT_FALSE(worker->is_running());
    ASSERT_NE(eng.get_observer_ring(), nullptr);
    EXPECT_TRUE(eng.get_observer_ring()->empty());
    EXPECT_TRUE(eng.is_halted());
    EXPECT_FALSE(eng.run_succeeded());

    // Halt and rollback compromise are sticky: retry cannot re-arm workers or
    // invoke strategy code again on this engine instance.
    EXPECT_NO_THROW(eng.run());
    EXPECT_EQ(strat->calls(), 1);
    EXPECT_TRUE(eng.is_halted());
}

TEST(Engine, ExtendedPreset_MMWorkerRuns)
{
    SilenceCout quiet;
    auto dh = make_bar_data(10);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<TestStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.threading = thread_preset::extended;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    // All 4 workers should have processed events
    EXPECT_GT(eng.get_logging_worker()->events_processed(), 0u);
    EXPECT_GT(eng.get_stats_worker()->events_processed(), 0u);
    EXPECT_GT(eng.get_risk_worker()->events_processed(), 0u);
}
