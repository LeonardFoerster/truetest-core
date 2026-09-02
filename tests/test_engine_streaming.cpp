#include <gtest/gtest.h>
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "providers/data_bridge.h"
#include "providers/local/csv_parser.h"
#include "helpers/mock_transport.h"
#include "engine/live_safety_session.h"
#include "execution/execution_adapter.h"
#include "execution/latency_model.h"
#include "providers/provider.h"
#include "threading/worker_watchdog.h"

#include <sstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <future>
#include <unistd.h>

namespace {
// RAII helper to silence cout during noisy engine runs.
// Anonymous namespace: same struct name exists in other test TUs with a
// different layout, and under LTO ODR-merging those definitions crashes at
// teardown. Anonymous-namespace scope gives each TU its own distinct type.
struct SilenceOutput {
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceOutput() : orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~SilenceOutput() { std::cout.rdbuf(orig); }
};

class FailingStreamingTransport final : public IDataTransport
{
public:
    bool open() override { open_ = true; return true; }
    void close() override { open_ = false; }
    bool is_open() const override { return open_; }
    bool is_streaming() const override { return true; }
    std::optional<std::string> read_line() override { return std::nullopt; }
    std::optional<std::string> read_line_blocking() override
    {
        if (!header_sent_)
        {
            header_sent_ = true;
            return "date,symbol,open,high,low,close,volume";
        }
        open_ = false;
        return std::nullopt;
    }
    transport_terminal_status terminal_status() const override
    {
        return header_sent_ ? transport_terminal_status::failed
                            : transport_terminal_status::unknown;
    }
private:
    bool open_ = false;
    bool header_sent_ = false;
};

class StreamingMutationSpyAdapter final : public IExecutionAdapter
{
public:
    void submit_order(const order_event&) override { ++submits; }
    bool cancel_order(std::uint64_t) override { ++cancels; return true; }
    bool poll_fills(std::vector<fill_event>&) override { return false; }
    int submits = 0;
    int cancels = 0;
};

class PassReconciler final : public IReconciler
{
public:
    std::string reconcile(const portfolio&, double) override { return {}; }
};

class PassKillSwitch final : public IKillSwitch
{
public:
    bool cancel_all_and_flatten(std::chrono::milliseconds) override
    {
        ++calls;
        return true;
    }
    int calls = 0;
};

class StreamingLiveProvider final : public IProvider
{
public:
    std::string name() const override { return "streaming-live-spy"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return true; }
    bool open() override { opened = true; return true; }
    void close() override { opened = false; }
    std::shared_ptr<IDataTransport> get_transport() override { return {}; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return adapter;
    }
    std::shared_ptr<IReconciler> get_reconciler() override { return reconciler; }
    std::shared_ptr<IKillSwitch> get_kill_switch() override { return kill_switch; }
    void quiesce_for_live_shutdown() override {}
    void finish_live_shutdown(live_shutdown_disposition disposition) override
    {
        close();
        last_disposition = disposition;
    }

    bool opened = false;
    live_shutdown_disposition last_disposition =
        live_shutdown_disposition::preserve_dead_man_switch;
    std::shared_ptr<StreamingMutationSpyAdapter> adapter =
        std::make_shared<StreamingMutationSpyAdapter>();
    std::shared_ptr<PassReconciler> reconciler =
        std::make_shared<PassReconciler>();
    std::shared_ptr<PassKillSwitch> kill_switch =
        std::make_shared<PassKillSwitch>();
};

class IdleFundingProvider final : public IProvider
{
public:
    std::string name() const override { return "idle-funding"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return false; }
    bool open() override { return true; }
    void close() override
    {
        if (publish_on_close && !closed)
        {
            const auto ts = std::chrono::system_clock::time_point{
                std::chrono::milliseconds{1'700'000'000'000LL}};
            close_publish_succeeded = ingress.try_publish(
                ts, "BTCUSDT", close_cash_delta);
        }
        if (overflow_on_close && !closed)
        {
            const auto ts = std::chrono::system_clock::time_point{
                std::chrono::milliseconds{1'700'000'000'000LL}};
            for (std::size_t i = 0;
                 i < ProviderFundingIngress::capacity; ++i)
            {
                (void)ingress.try_publish(
                    ts, "BTCUSDT", static_cast<double>(i) + 1.0);
            }
            (void)ingress.try_publish(ts, "BTCUSDT", 1.0);
        }
        closed = true;
    }
    std::shared_ptr<IDataTransport> get_transport() override { return {}; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return {};
    }
    ProviderFundingIngress* funding_ingress() noexcept override
    {
        return &ingress;
    }

    ProviderFundingIngress ingress;
    bool closed = false;
    bool publish_on_close = false;
    bool overflow_on_close = false;
    bool close_publish_succeeded = false;
    double close_cash_delta = 0.0;
};

class ReusableLivenessProvider final : public IProvider
{
public:
    std::string name() const override { return "reusable-liveness"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return false; }
    bool open() override { return true; }
    void close() override {}
    std::shared_ptr<IDataTransport> get_transport() override { return {}; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return {};
    }
    std::vector<liveness_source> get_liveness_sources() override
    {
        return {{"reusable-provider", &last_alive_ms, 10}};
    }

    std::atomic<std::int64_t> last_alive_ms{
        WorkerWatchdog::now_monotonic_ms()};
};

class SlowOnSecondRunStrategy final : public IStrategy
{
public:
    std::optional<order_event> on_market(const market_event&) override
    {
        ++calls;
        if (calls == 2)
            std::this_thread::sleep_for(std::chrono::milliseconds{1'200});
        return std::nullopt;
    }

    void set_position_open(const std::string&, bool) override {}

    int calls = 0;
};

class IdleBeforeHeaderTransport final : public IDataTransport
{
public:
    explicit IdleBeforeHeaderTransport(
        std::string header = "date,symbol,open,high,low,close,volume")
        : header_(std::move(header)) {}
    bool open() override { open_ = true; return true; }
    void close() override { open_ = false; }
    bool is_open() const override { return open_; }
    bool is_streaming() const override { return true; }
    std::optional<std::string> read_line() override { return std::nullopt; }
    bool supports_bounded_idle_read() const override { return true; }
    transport_read_result read_frame_until(
        std::string_view& out,
        std::chrono::steady_clock::time_point) override
    {
        if (step_++ == 0) return transport_read_result::idle;
        if (step_ == 2)
        {
            frame_ = header_;
            out = frame_;
            return transport_read_result::frame;
        }
        open_ = false;
        return transport_read_result::terminal;
    }
    transport_terminal_status terminal_status() const override
    {
        return transport_terminal_status::clean_eof;
    }
private:
    bool open_ = false;
    int step_ = 0;
    std::string header_;
    std::string frame_;
};

class DeterministicOneBarStreamingTransport final : public IDataTransport
{
public:
    bool open() override
    {
        index_ = 0;
        open_ = true;
        stop_requested_ = false;
        return true;
    }

    void close() override { open_ = false; }
    bool is_open() const override { return open_; }
    bool is_streaming() const override { return true; }

    std::optional<std::string> read_line() override
    {
        return read_line_blocking();
    }

    std::optional<std::string> read_line_blocking() override
    {
        if (!open_)
            return std::nullopt;
        if (index_ == 0)
        {
            ++index_;
            return "date,symbol,open,high,low,close,volume";
        }
        if (index_ == 1)
        {
            ++index_;
            return "2024-01-01,TEST,100,101,99,100,1000";
        }
        open_ = false;
        return std::nullopt;
    }

    void request_stop() override
    {
        stop_requested_ = true;
        open_ = false;
    }

    transport_terminal_status terminal_status() const override
    {
        if (stop_requested_)
            return transport_terminal_status::operator_stop;
        return index_ >= 2 ? transport_terminal_status::clean_eof
                           : transport_terminal_status::unknown;
    }

private:
    std::size_t index_ = 0;
    bool open_ = false;
    bool stop_requested_ = false;
};

class ThrowingStreamingStrategy final : public IStrategy
{
public:
    std::optional<order_event> on_market(const market_event&) override
    {
        ++calls_;
        throw std::runtime_error("deterministic streaming strategy failure");
    }

    void set_position_open(const std::string&, bool) override {}
    int calls() const noexcept { return calls_; }

private:
    int calls_ = 0;
};

struct ScopedStreamingLedger final
{
    explicit ScopedStreamingLedger(std::string_view suffix)
    {
        static std::atomic<std::uint64_t> sequence{0};
        path = "/tmp/truetest_engine_streaming_"
            + std::to_string(static_cast<unsigned long>(::getpid())) + "_"
            + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed))
            + "_" + std::string(suffix);
        std::remove(path.c_str());
    }

    ~ScopedStreamingLedger() { std::remove(path.c_str()); }

    std::string path;
};
}

// Test strategy: buys on bar 3, sells on bar 6
class StreamTestStrategy : public IStrategy
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

TEST(EngineStreaming, QuietStreamDrainsFundingBeforeFirstMarketFrame)
{
    SilenceOutput quiet;
    auto provider = std::make_shared<IdleFundingProvider>();
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<StreamTestStrategy>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.initial_balance = 1'000.0;
    cfg.provider = provider;
    engine eng(dh, ob, strat, std::move(cfg));

    const auto ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'700'000'000'000LL}};
    ASSERT_TRUE(provider->ingress.try_publish(ts, "BTCUSDT", 7.5));

    auto bridge = std::make_shared<DataBridge<bar_record>>(
        std::make_shared<IdleBeforeHeaderTransport>(),
        std::make_shared<CsvBarParser>(), bar_record_sink);
    const auto result = eng.run_streaming(bridge);
    EXPECT_TRUE(result.success());
    EXPECT_TRUE(provider->ingress.empty());

    truetest::ui::dashboard_snapshot snapshot;
    ASSERT_TRUE(eng.snapshot_dashboard(snapshot));
    EXPECT_DOUBLE_EQ(snapshot.cash, 1'007.5);
}

TEST(EngineStreaming, StartDrainsPrequeuedFundingBeforeMarketDispatch)
{
    auto provider = std::make_shared<IdleFundingProvider>();
    const auto ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'700'000'000'000LL}};
    ASSERT_TRUE(provider->ingress.try_publish(ts, "BTCUSDT", 2.25));

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.initial_balance = 1'000.0;
    cfg.provider = provider;
    engine eng(std::make_shared<data_handler>(), std::make_shared<orderbook>(),
               std::make_shared<StreamTestStrategy>(), std::move(cfg));

    EXPECT_FALSE(provider->ingress.empty());

    auto bridge = std::make_shared<DataBridge<bar_record>>(
        std::make_shared<IdleBeforeHeaderTransport>(),
        std::make_shared<CsvBarParser>(), bar_record_sink);
    const auto result = eng.run_streaming(bridge);
    EXPECT_TRUE(result.success());
    EXPECT_TRUE(provider->ingress.empty());
    truetest::ui::dashboard_snapshot snapshot;
    ASSERT_TRUE(eng.snapshot_dashboard(snapshot));
    EXPECT_DOUBLE_EQ(snapshot.cash, 1'002.25);
}

TEST(EngineStreaming, PrequeuedFundingIsPersistedInInlineEventLog)
{
    const std::string path = "/tmp/truetest_prequeued_funding_event_log.bin";
    std::remove(path.c_str());
    auto provider = std::make_shared<IdleFundingProvider>();
    const auto ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'700'000'000'000LL}};
    ASSERT_TRUE(provider->ingress.try_publish(ts, "BTCUSDT", -2.25));

    {
        engine_config cfg;
        cfg.mode = engine_mode::backtest;
        cfg.threading = thread_preset::inline_mode;
        cfg.disable_pinning = true;
        cfg.initial_balance = 1'000.0;
        cfg.provider = provider;
        cfg.event_log_path = path;
        cfg.compress_log = false;
        engine eng(std::make_shared<data_handler>(),
                   std::make_shared<orderbook>(),
                   std::make_shared<StreamTestStrategy>(), std::move(cfg));
        auto bridge = std::make_shared<DataBridge<bar_record>>(
            std::make_shared<IdleBeforeHeaderTransport>(),
            std::make_shared<CsvBarParser>(), bar_record_sink);
        EXPECT_TRUE(eng.run_streaming(bridge).success());
    }

    EventReplayer replayer(path);
    auto ev = replayer.next();
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->get_type(), event_type::funding);
    EXPECT_DOUBLE_EQ(static_cast<const funding_event&>(*ev).get_cash_delta(),
                     -2.25);
    EXPECT_FALSE(replayer.has_next());
    std::remove(path.c_str());
}

TEST(EngineStreaming, PrequeuedFundingReachesEveryThreadedAnalyticsAndDurableLog)
{
    SilenceOutput quiet;
    const auto ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'700'000'000'000LL}};

    for (const auto preset : {thread_preset::light, thread_preset::standard,
                              thread_preset::full, thread_preset::extended})
    {
        SCOPED_TRACE(static_cast<int>(preset));
        const bool supports_log = preset != thread_preset::light;
        const std::string path =
            "/tmp/truetest_prequeued_funding_threaded_"
            + std::to_string(static_cast<int>(preset)) + ".bin";
        std::remove(path.c_str());

        auto provider = std::make_shared<IdleFundingProvider>();
        ASSERT_TRUE(provider->ingress.try_publish(ts, "BTCUSDT", -2.25));

        double reported_funding = 0.0;
        {
            engine_config cfg;
            cfg.mode = engine_mode::backtest;
            cfg.threading = preset;
            cfg.disable_pinning = true;
            cfg.initial_balance = 1'000.0;
            cfg.provider = provider;
            if (supports_log)
                cfg.event_log_path = path;
            cfg.compress_log = false;
            engine eng(std::make_shared<data_handler>(),
                       std::make_shared<orderbook>(),
                       std::make_shared<StreamTestStrategy>(), std::move(cfg));

            auto bridge = std::make_shared<DataBridge<bar_record>>(
                std::make_shared<IdleBeforeHeaderTransport>(),
                std::make_shared<CsvBarParser>(), bar_record_sink);
            ASSERT_TRUE(eng.run_streaming(bridge).success());
            reported_funding = eng.get_analytics().total_funding_pnl();
        }

        EXPECT_DOUBLE_EQ(reported_funding, -2.25);
        if (supports_log)
        {
            EventReplayer replayer(path);
            auto ev = replayer.next();
            ASSERT_NE(ev, nullptr);
            ASSERT_EQ(ev->get_type(), event_type::funding);
            EXPECT_DOUBLE_EQ(
                static_cast<const funding_event&>(*ev).get_cash_delta(),
                -2.25);
            EXPECT_FALSE(replayer.has_next());
        }
        std::remove(path.c_str());
    }
}

TEST(EngineStreaming, QuietTickStreamUsesFundingIdleDrain)
{
    auto provider = std::make_shared<IdleFundingProvider>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.initial_balance = 1'000.0;
    cfg.provider = provider;
    engine eng(std::make_shared<data_handler>(), nullptr,
               std::make_shared<StreamTestStrategy>(), std::move(cfg));

    const auto ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'700'000'000'000LL}};
    ASSERT_TRUE(provider->ingress.try_publish(ts, "BTCUSDT", 4.0));
    auto bridge = std::make_shared<DataBridge<tick_record>>(
        std::make_shared<IdleBeforeHeaderTransport>(
            "timestamp_ms,symbol,price,quantity,side"),
        std::make_shared<CsvTickParser>(), tick_record_sink);
    const auto result = eng.run_streaming(bridge);
    EXPECT_TRUE(result.success());
    EXPECT_TRUE(provider->ingress.empty());
    truetest::ui::dashboard_snapshot snapshot;
    ASSERT_TRUE(eng.snapshot_dashboard(snapshot));
    EXPECT_DOUBLE_EQ(snapshot.cash, 1'004.0);
}

TEST(EngineStreaming, PrelatchedFundingOverflowRefusesConstruction)
{
    auto provider = std::make_shared<IdleFundingProvider>();
    const auto ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'700'000'000'000LL}};
    for (std::size_t i = 0; i < ProviderFundingIngress::capacity; ++i)
        ASSERT_TRUE(provider->ingress.try_publish(
            ts, "BTCUSDT", static_cast<double>(i) + 1.0));
    ASSERT_FALSE(provider->ingress.try_publish(ts, "BTCUSDT", 65.0));

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.initial_balance = 1'000.0;
    cfg.provider = provider;
    EXPECT_THROW(
        engine(std::make_shared<data_handler>(), std::make_shared<orderbook>(),
               std::make_shared<StreamTestStrategy>(), std::move(cfg)),
        std::runtime_error);
}

TEST(EngineStreaming, RuntimeFundingOverflowHaltsBeforeMarketDispatch)
{
    SilenceOutput quiet;
    auto provider = std::make_shared<IdleFundingProvider>();
    auto strategy = std::make_shared<StreamTestStrategy>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.initial_balance = 1'000.0;
    cfg.provider = provider;
    engine eng(std::make_shared<data_handler>(), std::make_shared<orderbook>(),
               strategy, std::move(cfg));

    const auto ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'700'000'000'000LL}};
    for (std::size_t i = 0; i < ProviderFundingIngress::capacity; ++i)
        ASSERT_TRUE(provider->ingress.try_publish(
            ts, "BTCUSDT", static_cast<double>(i) + 1.0));
    ASSERT_FALSE(provider->ingress.try_publish(ts, "BTCUSDT", 65.0));

    auto bridge = std::make_shared<DataBridge<bar_record>>(
        std::make_shared<IdleBeforeHeaderTransport>(),
        std::make_shared<CsvBarParser>(), bar_record_sink);
    const auto result = eng.run_streaming(bridge);
    EXPECT_FALSE(result.success());
    EXPECT_EQ(result.termination, stream_termination::engine_halt);
    EXPECT_EQ(strategy->get_call_count(), 0);
    EXPECT_FALSE(eng.run_succeeded());
    EXPECT_TRUE(provider->ingress.empty());
    truetest::ui::dashboard_snapshot snapshot;
    ASSERT_TRUE(eng.snapshot_dashboard(snapshot));
    constexpr double retained_sum =
        (ProviderFundingIngress::capacity
         * (ProviderFundingIngress::capacity + 1)) / 2.0;
    EXPECT_DOUBLE_EQ(snapshot.cash, 1'000.0 + retained_sum);
}

TEST(EngineStreaming, FinalDrainAppliesFundingAfterProviderProducerStops)
{
    SilenceOutput quiet;
    const std::string path =
        "/tmp/truetest_final_provider_funding_event_log.bin";
    std::remove(path.c_str());
    auto provider = std::make_shared<IdleFundingProvider>();
    provider->publish_on_close = true;
    provider->close_cash_delta = 3.25;
    auto session = std::make_shared<LiveSafetySession>(
        provider, false, std::chrono::milliseconds{100});
    ASSERT_TRUE(session->open_provider());

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.initial_balance = 1'000.0;
    cfg.provider = provider;
    cfg.live_safety_session = session;
    cfg.event_log_path = path;
    cfg.compress_log = false;
    engine eng(std::make_shared<data_handler>(), std::make_shared<orderbook>(),
               std::make_shared<StreamTestStrategy>(), std::move(cfg));

    auto bridge = std::make_shared<DataBridge<bar_record>>(
        std::make_shared<IdleBeforeHeaderTransport>(),
        std::make_shared<CsvBarParser>(), bar_record_sink);
    const auto result = eng.run_streaming(bridge);
    EXPECT_TRUE(result.success());
    EXPECT_TRUE(provider->close_publish_succeeded);

    truetest::ui::dashboard_snapshot snapshot;
    ASSERT_TRUE(eng.snapshot_dashboard(snapshot));
    EXPECT_DOUBLE_EQ(snapshot.cash, 1'003.25);

    EventReplayer replayer(path);
    auto ev = replayer.next();
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->get_type(), event_type::funding);
    EXPECT_DOUBLE_EQ(static_cast<const funding_event&>(*ev).get_cash_delta(),
                     3.25);
    EXPECT_FALSE(replayer.has_next());
    std::remove(path.c_str());
}

TEST(EngineStreaming, FinalFundingOverflowPreventsReservedLedgerSeal)
{
    SilenceOutput quiet;
    const std::string path =
        "/tmp/truetest_final_provider_funding_overflow_event_log.bin";
    std::remove(path.c_str());
    auto reservation = DurableEventLogReservation::acquire(path);
    auto logger = std::make_unique<EventLogger>(
        path, false, 0, 5, reservation);
    logger->verify_durable_ready();

    auto provider = std::make_shared<IdleFundingProvider>();
    provider->overflow_on_close = true;
    auto session = std::make_shared<LiveSafetySession>(
        provider, false, std::chrono::milliseconds{100});
    ASSERT_TRUE(session->open_provider());

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::standard;
    cfg.disable_pinning = true;
    cfg.show_progress = false;
    cfg.initial_balance = 1'000.0;
    cfg.provider = provider;
    cfg.live_safety_session = session;
    cfg.event_log_path = path;
    cfg.compress_log = false;
    cfg.event_log_reservation = reservation;
    {
        engine eng(std::make_shared<data_handler>(),
                   std::make_shared<orderbook>(),
                   std::make_shared<StreamTestStrategy>(), std::move(cfg),
                   std::move(logger));
        auto bridge = std::make_shared<DataBridge<bar_record>>(
            std::make_shared<IdleBeforeHeaderTransport>(),
            std::make_shared<CsvBarParser>(), bar_record_sink);
        const auto result = eng.run_streaming(bridge);
        EXPECT_FALSE(result.success());
        EXPECT_TRUE(eng.is_halted());
        EXPECT_FALSE(eng.run_succeeded());
    }

    EventReplayer inspection(reservation->open_path());
    EXPECT_FALSE(inspection.file_finalized());
    std::remove(path.c_str());
}

TEST(EngineLifecycle, WatchdogRestartsForCleanSecondRun)
{
    SilenceOutput quiet;
    auto provider = std::make_shared<ReusableLivenessProvider>();
    auto strategy = std::make_shared<SlowOnSecondRunStrategy>();
    auto data = std::make_shared<data_handler>();
    data->load_into_queue(
        "2024-01-01", "TEST", 100.0, 101.0, 99.0, 100.0, 1000);

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.show_progress = false;
    cfg.provider = provider;
    engine eng(data, std::make_shared<orderbook>(), strategy, std::move(cfg));

    eng.run();
    ASSERT_TRUE(eng.run_succeeded());
    ASSERT_EQ(strategy->calls, 1);

    provider->last_alive_ms.store(
        WorkerWatchdog::now_monotonic_ms() - 1'000,
        std::memory_order_release);
    eng.run();

    EXPECT_EQ(strategy->calls, 2);
    EXPECT_TRUE(eng.is_halted());
    EXPECT_FALSE(eng.run_succeeded());
}

// Tick-counting strategy: just counts ticks, never trades
class TickCountStrategy : public IStrategy
{
    std::atomic<int> tick_count_{0};
public:
    int get_tick_count() const { return tick_count_.load(); }

    std::optional<order_event> on_market(const market_event&) override
    {
        return std::nullopt;
    }

    std::optional<order_event> on_tick(const tick_event&) override
    {
        tick_count_++;
        return std::nullopt;
    }

    void set_position_open(const std::string&, bool) override {}
};

class NotifyingOneShotStrategy : public IStrategy
{
public:
    explicit NotifyingOneShotStrategy(std::promise<void>& fired)
        : fired_(fired) {}

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        if (did_fire_) return std::nullopt;
        did_fire_ = true;
        fired_.set_value();
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy,
                           1.0, mkt.get_close());
    }
    void set_position_open(const std::string&, bool) override {}
    bool did_fire() const { return did_fire_; }

private:
    std::promise<void>& fired_;
    bool did_fire_ = false;
};

static std::vector<std::string> make_bar_lines(int n)
{
    // Header + n data lines
    std::vector<std::string> lines;
    lines.push_back("date,symbol,open,high,low,close,volume");
    for (int i = 0; i < n; ++i)
    {
        lines.push_back("2024-01-01,TEST," +
            std::to_string(100.0 + i) + "," +
            std::to_string(105.0 + i) + "," +
            std::to_string(95.0 + i) + "," +
            std::to_string(102.0 + i) + ",1000");
    }
    return lines;
}

static std::vector<std::string> make_tick_lines(int n)
{
    std::vector<std::string> lines;
    lines.push_back("timestamp_ms,symbol,price,quantity,side");
    for (int i = 0; i < n; ++i)
    {
        lines.push_back(std::to_string(1000 + i * 100) + ",TEST," +
            std::to_string(100.0 + i * 0.1) + "," +
            std::to_string(10 + i) + ",B");
    }
    return lines;
}

// --- Bar streaming tests ---

TEST(EngineStreaming, BarStreamProcessesAllRecords)
{
    SilenceOutput silence;

    auto transport = std::make_shared<MockStreamingTransport>();
    auto parser = std::make_shared<CsvBarParser>();
    auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

    auto dh = std::make_shared<data_handler>();
    auto strategy = std::make_shared<StreamTestStrategy>();

    engine_config cfg;
    cfg.seed = 42;
    engine eng(dh, nullptr, strategy, std::move(cfg));

    auto lines = make_bar_lines(10);

    // Feed data from a background thread
    std::thread feeder([&]() {
        // Give the engine a moment to start
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (const auto& line : lines)
        {
            transport->enqueue(line);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // Signal end of data
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        transport->request_stop();
    });

    eng.run_streaming(bridge);
    feeder.join();

    // D-06: default retain_streamed=false — series does not grow; strategy still sees events.
    EXPECT_EQ(dh->bar_count(), 0u);
    // Strategy should have been called 10 times
    EXPECT_EQ(strategy->get_call_count(), 10);
}

TEST(EngineStreaming, BarStreamStrategyGeneratesSignals)
{
    SilenceOutput silence;

    auto transport = std::make_shared<MockStreamingTransport>();
    auto parser = std::make_shared<CsvBarParser>();
    auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

    auto dh = std::make_shared<data_handler>();
    auto strategy = std::make_shared<StreamTestStrategy>();

    engine_config cfg;
    cfg.seed = 42;
    engine eng(dh, nullptr, strategy, std::move(cfg));

    auto lines = make_bar_lines(10);

    std::thread feeder([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (const auto& line : lines)
        {
            transport->enqueue(line);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        transport->request_stop();
    });

    eng.run_streaming(bridge);
    feeder.join();

    // Strategy buys at bar 3 and sells at bar 6 - should see trades
    EXPECT_GT(strategy->get_call_count(), 5);
}

TEST(EngineStreaming, BarStreamStopCausesReturn)
{
    SilenceOutput silence;

    auto transport = std::make_shared<MockStreamingTransport>();
    auto parser = std::make_shared<CsvBarParser>();
    auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

    auto dh = std::make_shared<data_handler>();
    auto strategy = std::make_shared<StreamTestStrategy>();

    engine_config cfg;
    cfg.seed = 42;
    engine eng(dh, nullptr, strategy, std::move(cfg));

    // Enqueue header only, then stop after a short delay
    transport->enqueue("date,symbol,open,high,low,close,volume");

    std::thread stopper([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        bridge->stop();
    });

    auto start = std::chrono::steady_clock::now();
    eng.run_streaming(bridge);
    auto elapsed = std::chrono::steady_clock::now() - start;
    stopper.join();

    // Should return promptly (< 500ms)
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
    // No data should have been processed
    EXPECT_EQ(dh->bar_count(), 0u);
}

TEST(EngineStreaming, LiveOperatorStopDoesNotForcePendingVenueMutation)
{
    SilenceOutput silence;
    auto transport = std::make_shared<MockStreamingTransport>();
    transport->enqueue("date,symbol,open,high,low,close,volume");
    transport->enqueue("2024-01-01,TEST,100,101,99,100,1000");
    auto bridge = std::make_shared<DataBridge<bar_record>>(
        transport, std::make_shared<CsvBarParser>(), bar_record_sink);
    auto provider = std::make_shared<StreamingLiveProvider>();
    auto session = std::make_shared<LiveSafetySession>(
        provider, true, std::chrono::milliseconds{50});
    ASSERT_TRUE(session->open_provider());

    engine_config cfg;
    cfg.mode = engine_mode::live;
    cfg.provider = provider;
    cfg.live_safety_session = session;
    cfg.reconciler = provider->reconciler;
    cfg.latency_model = std::make_shared<FixedLatencyModel>(
        std::chrono::duration_cast<latency_duration>(std::chrono::hours(1)));
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    std::promise<void> fired;
    auto fired_future = fired.get_future();
    std::atomic<bool> fired_before_timeout{false};
    auto strategy = std::make_shared<NotifyingOneShotStrategy>(fired);
    engine eng(std::make_shared<data_handler>(), nullptr, strategy,
               std::move(cfg));

    std::thread stopper([&] {
        fired_before_timeout.store(
            fired_future.wait_for(std::chrono::seconds(2))
                == std::future_status::ready,
            std::memory_order_release);
        bridge->stop();
    });

    const auto result = eng.run_streaming(bridge);
    stopper.join();

    EXPECT_EQ(result.termination, stream_termination::operator_stop);
    EXPECT_TRUE(fired_before_timeout.load(std::memory_order_acquire));
    EXPECT_TRUE(strategy->did_fire());
    EXPECT_EQ(provider->adapter->submits, 0);
    EXPECT_EQ(provider->adapter->cancels, 0);
    EXPECT_EQ(eng.get_order_tracker().active_count(), 0u);
}

TEST(EngineStreaming, BarStreamBatchTransportFallback)
{
    SilenceOutput silence;

    // Use MockBatchTransport (non-streaming) through run_streaming -
    // it should still work since DataBridge::run_streaming falls back to read_line()
    auto lines = make_bar_lines(5);
    auto transport = std::make_shared<MockBatchTransport>(lines);
    auto parser = std::make_shared<CsvBarParser>();
    auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

    auto dh = std::make_shared<data_handler>();
    auto strategy = std::make_shared<StreamTestStrategy>();

    engine_config cfg;
    cfg.seed = 42;
    engine eng(dh, nullptr, strategy, std::move(cfg));

    eng.run_streaming(bridge);

    // D-06: process without retaining into series by default
    EXPECT_EQ(dh->bar_count(), 0u);
    EXPECT_EQ(strategy->get_call_count(), 5);
}

TEST(EngineStreaming, TransportFailureMarksRunFailedAndPropagatesResult)
{
    SilenceOutput silence;
    auto bridge = std::make_shared<DataBridge<bar_record>>(
        std::make_shared<FailingStreamingTransport>(),
        std::make_shared<CsvBarParser>(), bar_record_sink);
    auto dh = std::make_shared<data_handler>();
    auto strategy = std::make_shared<StreamTestStrategy>();
    engine_config cfg;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    engine eng(dh, nullptr, strategy, std::move(cfg));

    const auto result = eng.run_streaming(bridge);

    EXPECT_EQ(result.termination, stream_termination::transport_failure);
    EXPECT_FALSE(result.success());
    EXPECT_FALSE(eng.run_succeeded());
}

TEST(EngineStreaming,
     StrategyRuntimeFailureStopsWorkersDrainsRingsAndRefusesLedgerSeal)
{
    SilenceOutput silence;
    ScopedStreamingLedger ledger("strategy_runtime_failure.bin");
    auto reservation = DurableEventLogReservation::acquire(ledger.path);
    auto logger = std::make_unique<EventLogger>(
        ledger.path, false, 0, 5, reservation);
    logger->verify_durable_ready();

    auto strategy = std::make_shared<ThrowingStreamingStrategy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::standard;
    cfg.disable_pinning = true;
    cfg.show_progress = false;
    cfg.event_log_path = ledger.path;
    cfg.compress_log = false;
    cfg.event_log_reservation = reservation;

    {
        engine eng(std::make_shared<data_handler>(),
                   std::make_shared<orderbook>(), strategy, std::move(cfg),
                   std::move(logger));
        auto bridge = std::make_shared<DataBridge<bar_record>>(
            std::make_shared<DeterministicOneBarStreamingTransport>(),
            std::make_shared<CsvBarParser>(), bar_record_sink);

        const auto result = eng.run_streaming(bridge);

        EXPECT_EQ(result.termination, stream_termination::runtime_failure);
        EXPECT_FALSE(result.success());
        EXPECT_EQ(strategy->calls(), 1);
        EXPECT_TRUE(eng.is_halted());
        EXPECT_FALSE(eng.run_succeeded());

        ASSERT_NE(eng.get_logging_worker(), nullptr);
        ASSERT_NE(eng.get_risk_stats_worker(), nullptr);
        EXPECT_FALSE(eng.get_logging_worker()->is_running());
        EXPECT_FALSE(eng.get_risk_stats_worker()->is_running());
        ASSERT_NE(eng.get_logging_ring(), nullptr);
        ASSERT_NE(eng.get_risk_stats_ring(), nullptr);
        EXPECT_TRUE(eng.get_logging_ring()->empty());
        EXPECT_TRUE(eng.get_risk_stats_ring()->empty());

        // Halt is terminal for this engine instance. A fresh source cannot
        // re-arm workers or re-enter strategy code after the failed callback.
        auto retry_bridge = std::make_shared<DataBridge<bar_record>>(
            std::make_shared<DeterministicOneBarStreamingTransport>(),
            std::make_shared<CsvBarParser>(), bar_record_sink);
        const auto retry = eng.run_streaming(retry_bridge);
        EXPECT_EQ(retry.termination, stream_termination::engine_halt);
        EXPECT_EQ(strategy->calls(), 1);
        EXPECT_TRUE(eng.is_halted());
        EXPECT_FALSE(eng.get_logging_worker()->is_running());
        EXPECT_FALSE(eng.get_risk_stats_worker()->is_running());
        EXPECT_TRUE(eng.get_logging_ring()->empty());
        EXPECT_TRUE(eng.get_risk_stats_ring()->empty());
    }

    EventReplayer inspection(reservation->open_path());
    EXPECT_FALSE(inspection.file_finalized());
}

TEST(EngineStreaming, NullBridgesAreRejectedBeforeWorkerStartup)
{
    SilenceOutput silence;
    engine_config cfg;
    cfg.threading = thread_preset::standard;
    cfg.disable_pinning = true;
    cfg.show_progress = false;
    engine eng(std::make_shared<data_handler>(),
               std::make_shared<orderbook>(),
               std::make_shared<StreamTestStrategy>(), std::move(cfg));

    EXPECT_THROW(
        (void)eng.run_streaming(
            std::shared_ptr<DataBridge<bar_record>>{}),
        std::runtime_error);
    EXPECT_THROW(
        (void)eng.run_streaming(
            std::shared_ptr<DataBridge<tick_record>>{}),
        std::runtime_error);
    EXPECT_THROW(
        (void)eng.run_streaming(
            std::shared_ptr<DataBridge<provider::event>>{}),
        std::runtime_error);

    EXPECT_EQ(eng.get_logging_worker(), nullptr);
    EXPECT_EQ(eng.get_risk_stats_worker(), nullptr);
    EXPECT_EQ(eng.get_logging_ring(), nullptr);
    EXPECT_EQ(eng.get_risk_stats_ring(), nullptr);
    EXPECT_FALSE(eng.is_halted());
}

TEST(EngineLifecycle, NullDataHandlerIsRejectedBeforeTickWorkerStartup)
{
    SilenceOutput silence;
    engine_config cfg;
    cfg.threading = thread_preset::standard;
    cfg.disable_pinning = true;
    cfg.show_progress = false;
    engine eng(std::shared_ptr<data_handler>{},
               std::make_shared<orderbook>(),
               std::make_shared<StreamTestStrategy>(), std::move(cfg));

    EXPECT_THROW(eng.run_tick_data(), std::runtime_error);
    EXPECT_EQ(eng.get_logging_worker(), nullptr);
    EXPECT_EQ(eng.get_risk_stats_worker(), nullptr);
    EXPECT_EQ(eng.get_logging_ring(), nullptr);
    EXPECT_EQ(eng.get_risk_stats_ring(), nullptr);
    EXPECT_FALSE(eng.is_halted());
}

TEST(EngineStreaming, BridgeReuseAfterEngineDestructionHasNoStaleHaltOwner)
{
    SilenceOutput silence;
    auto bridge = std::make_shared<DataBridge<bar_record>>(
        std::make_shared<DeterministicOneBarStreamingTransport>(),
        std::make_shared<CsvBarParser>(), bar_record_sink);
    auto strategy = std::make_shared<StreamTestStrategy>();

    {
        engine_config cfg;
        cfg.threading = thread_preset::inline_mode;
        cfg.disable_pinning = true;
        cfg.show_progress = false;
        engine eng(std::make_shared<data_handler>(),
                   std::make_shared<orderbook>(), strategy, std::move(cfg));
        ASSERT_EQ(eng.run_streaming(bridge).termination,
                  stream_termination::clean_eof);
        ASSERT_EQ(strategy->get_call_count(), 1);
        eng.trigger_halt("lifetime regression sentinel");
    }

    std::size_t callbacks = 0;
    const auto result = bridge->run_streaming(
        std::make_shared<data_handler>(),
        [&](const bar_record&) { ++callbacks; });
    EXPECT_EQ(result.termination, stream_termination::clean_eof);
    EXPECT_EQ(callbacks, 1u);
}

// --- Tick streaming tests ---

TEST(EngineStreaming, TickStreamProcessesAllRecords)
{
    SilenceOutput silence;

    auto transport = std::make_shared<MockStreamingTransport>();
    auto parser = std::make_shared<CsvTickParser>();
    auto bridge = std::make_shared<DataBridge<tick_record>>(transport, parser, tick_record_sink);

    auto dh = std::make_shared<data_handler>();
    auto strategy = std::make_shared<TickCountStrategy>();

    engine_config cfg;
    cfg.seed = 42;
    engine eng(dh, nullptr, strategy, std::move(cfg));

    auto lines = make_tick_lines(8);

    // Pre-enqueue header so run_streaming's read_line() finds it immediately
    transport->enqueue(lines[0]);

    std::thread feeder([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (size_t i = 1; i < lines.size(); ++i)
        {
            transport->enqueue(lines[i]);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        transport->request_stop();
    });

    eng.run_streaming(bridge);
    feeder.join();

    // D-06: ticks processed via callback; series not retained by default
    EXPECT_EQ(dh->tick_count(), 0u);
    EXPECT_EQ(strategy->get_tick_count(), 8);
}

TEST(EngineStreaming, TickStreamStopCausesReturn)
{
    SilenceOutput silence;

    auto transport = std::make_shared<MockStreamingTransport>();
    auto parser = std::make_shared<CsvTickParser>();
    auto bridge = std::make_shared<DataBridge<tick_record>>(transport, parser, tick_record_sink);

    auto dh = std::make_shared<data_handler>();
    auto strategy = std::make_shared<TickCountStrategy>();

    engine_config cfg;
    cfg.seed = 42;
    engine eng(dh, nullptr, strategy, std::move(cfg));

    // Enqueue header only
    transport->enqueue("timestamp_ms,symbol,price,quantity,side");

    std::thread stopper([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        bridge->stop();
    });

    auto start = std::chrono::steady_clock::now();
    eng.run_streaming(bridge);
    auto elapsed = std::chrono::steady_clock::now() - start;
    stopper.join();

    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
    EXPECT_EQ(dh->tick_count(), 0u);
}
