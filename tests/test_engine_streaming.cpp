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

#include <sstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
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

class PrivateLiveLedgerFile
{
public:
    explicit PrivateLiveLedgerFile(std::string_view name)
    {
        std::string pattern = "/tmp/truetest_engine_live_ledger_XXXXXX";
        char* created = ::mkdtemp(pattern.data());
        if (created == nullptr)
            throw std::runtime_error(
                "cannot create private engine ledger test directory");
        directory_ = created;
        if (::chmod(directory_.c_str(), S_IRWXU) != 0)
        {
            (void)::rmdir(directory_.c_str());
            throw std::runtime_error(
                "cannot secure engine ledger test directory");
        }
        path_ = directory_ + "/" + std::string(name);
    }

    ~PrivateLiveLedgerFile()
    {
        (void)::unlink((path_ + ".partial").c_str());
        (void)::unlink(path_.c_str());
        (void)::rmdir(directory_.c_str());
    }

    const std::string& path() const noexcept { return path_; }

private:
    std::string directory_;
    std::string path_;
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
        // This streaming test exercises operator stop before a venue order is
        // eligible.  It deliberately has no private execution protocol, so
        // it must not masquerade as a live adapter under the unified-ingress
        // construction gate.
        return {};
    }
    std::shared_ptr<IReconciler> get_reconciler() override { return reconciler; }
    std::shared_ptr<IKillSwitch> get_kill_switch() override { return kill_switch; }
    void quiesce_for_live_shutdown() override {}
    void finish_live_shutdown(live_shutdown_disposition disposition) override
    {
        close();
        last_disposition = disposition;
    }
    bool private_execution_producer_joined() const noexcept override
    {
        // This fixture owns no private producer; its synchronous finish is
        // the test-only join proof required for authoritative ledger close.
        return !opened;
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
    bool close_publish_succeeded = false;
    double close_cash_delta = 0.0;
};

// Minimal engine-thread resolver used to prove the unified private FIFO
// semantics without a network transport.  It deliberately does not synthesize
// fills on the producer side: the engine must call resolve/ack in FIFO order.
class UnifiedIngressSpyAdapter final : public IExecutionAdapter,
                                       public IAsyncSubmitSupport
{
public:
    void submit_order(const order_event&) override { ++submits; }
    bool cancel_order(std::uint64_t) override { ++cancels; return true; }
    bool modify_order(std::uint64_t, double, double) override
    {
        ++modifies;
        return true;
    }
    bool poll_fills(std::vector<fill_event>& out) override
    {
        ++legacy_poll_calls;
        if (throw_on_legacy_poll)
            throw std::runtime_error("legacy poll failure");
        if (legacy_fills.empty())
            return false;
        out.insert(out.end(), legacy_fills.begin(), legacy_fills.end());
        legacy_fills.clear();
        return true;
    }
    bool supports_async_submit() const override { return true; }
    IAsyncSubmitSupport* get_async_support() override { return this; }

    void set_unknown_fill_handler(unknown_fill_handler) override {}
    void clear_unknown_fill_handler() override {}
    bool poll_synth_meta(std::vector<synth_meta>&) override { return false; }
    bool poll_submit_results(std::vector<submit_result>&) override { return false; }

    private_execution_resolution resolve_private_execution(
        private_execution_record& record) override
    {
        if (record.client_order_id_view() == "foreign")
            return private_execution_resolution::untracked;
        if (record.client_order_id_view() != "tt-1")
            return private_execution_resolution::fatal;
        record.engine_order_id = 1;
        record.remaining_qty = record.k == private_execution_record::kind::partial_fill
            ? 1.0 : 0.0;
        return private_execution_resolution::tracked;
    }

    bool acknowledge_private_terminal(std::uint64_t sequence) override
    {
        acknowledged_terminal_sequences.push_back(sequence);
        return true;
    }

    bool commit_private_execution(
        const private_execution_reservation& reservation) override
    {
        committed_sequences.push_back(reservation.source_sequence);
        return reservation.valid();
    }

    bool rollback_private_execution(
        const private_execution_reservation& reservation) noexcept override
    {
        rolled_back_sequences.push_back(reservation.source_sequence);
        return reservation.valid();
    }

    bool has_unresolved_private_lifecycle() const override { return false; }

    void fail_private_execution_admission() noexcept override
    {
        admission_failed = true;
    }

    int submits = 0;
    int cancels = 0;
    int modifies = 0;
    int legacy_poll_calls = 0;
    bool throw_on_legacy_poll = false;
    std::vector<fill_event> legacy_fills;
    bool admission_failed = false;
    std::vector<std::uint64_t> acknowledged_terminal_sequences;
    std::vector<std::uint64_t> committed_sequences;
    std::vector<std::uint64_t> rolled_back_sequences;
};

class UnifiedIngressProvider final : public IProvider
{
public:
    std::string name() const override { return "unified-ingress-spy"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return true; }
    bool open() override { opened = true; return true; }
    void close() override
    {
        if (!closed && publish_terminal_on_close)
        {
            private_execution_record record;
            record.k = private_execution_record::kind::full_fill;
            record.event_time_ms = 1'700'000'000'000LL;
            record.side = order_side::buy;
            record.last_fill_qty = 1.0;
            record.last_fill_price = 100.0;
            record.cumulative_qty = 1.0;
            record.remaining_qty = 0.0;
            record.cumulative_reported = true;
            (void)private_execution_record::copy_text(
                record.symbol, record.symbol_size, "BTCUSDT");
            (void)private_execution_record::copy_text(
                record.client_order_id, record.client_order_id_size, "tt-1");
            (void)private_execution_record::copy_text(
                record.exchange_order_id, record.exchange_order_id_size, "ex-1");
            (void)private_execution_record::copy_text(
                record.execution_id, record.execution_id_size, "trade-1");
            close_publish_succeeded = ingress.try_publish(record);
        }
        if (!closed)
        {
            for (auto record : close_records)
                close_records_publish_succeeded =
                    ingress.try_publish(record) && close_records_publish_succeeded;
        }
        closed = true;
        joined = true;
        opened = false;
    }
    void finish_live_shutdown(live_shutdown_disposition) override { close(); }
    std::shared_ptr<IDataTransport> get_transport() override { return {}; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return adapter;
    }
    ProviderExecutionIngress* execution_ingress() noexcept override
    {
        return &ingress;
    }
    bool private_execution_producer_joined() const noexcept override
    {
        return joined;
    }

    ProviderExecutionIngress ingress;
    std::shared_ptr<UnifiedIngressSpyAdapter> adapter =
        std::make_shared<UnifiedIngressSpyAdapter>();
    bool opened = false;
    bool closed = false;
    bool joined = false;
    bool publish_terminal_on_close = false;
    bool close_publish_succeeded = false;
    std::vector<private_execution_record> close_records;
    bool close_records_publish_succeeded = true;
};

class FillCountingStrategy final : public IStrategy
{
public:
    std::optional<order_event> on_market(const market_event&) override
    {
        return std::nullopt;
    }
    void on_fill(const fill_event&, std::uint64_t) override { ++fills; }
    void set_position_open(const std::string&, bool) override {}
    int fills = 0;
};

class OneShotL2EntryStrategy final : public IStrategy
{
public:
    std::optional<order_event> on_market(const market_event&) override
    {
        return std::nullopt;
    }

    std::optional<order_event> on_l2_update(const l2_update_event& update) override
    {
        if (emitted)
            return std::nullopt;
        emitted = true;
        return order_event(update.get_timestamp(), update.get_symbol(),
                           order_type::market, order_side::buy, 1.0,
                           update.get_price());
    }

    void set_position_open(const std::string&, bool) override {}
    bool emitted = false;
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

TEST(EngineStreaming, ConstructorDrainsPrequeuedFunding)
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

    EXPECT_TRUE(provider->ingress.empty());
    truetest::ui::dashboard_snapshot snapshot;
    ASSERT_TRUE(eng.snapshot_dashboard(snapshot));
    EXPECT_DOUBLE_EQ(snapshot.cash, 1'002.25);
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
}

TEST(EngineStreaming, UnifiedPrivateFundingDrainsBeforeFirstMarketFrame)
{
    SilenceOutput quiet;
    auto provider = std::make_shared<UnifiedIngressProvider>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.initial_balance = 1'000.0;
    cfg.provider = provider;
    engine eng(std::make_shared<data_handler>(), std::make_shared<orderbook>(),
               std::make_shared<StreamTestStrategy>(), std::move(cfg));
    // This test injects directly on the engine-side fake; no reader is
    // running at EOS, so its producer-join proof is already true.
    provider->joined = true;

    private_execution_record funding;
    funding.k = private_execution_record::kind::funding;
    funding.event_time_ms = 1'700'000'000'000LL;
    funding.cash_delta = 7.5;
    ASSERT_TRUE(private_execution_record::copy_text(
        funding.symbol, funding.symbol_size, "BTCUSDT"));
    ASSERT_TRUE(provider->ingress.try_publish(funding));

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

TEST(EngineStreaming, UnifiedLegacyFillIsRejectedBeforeFreshVenueSubmit)
{
    auto provider = std::make_shared<UnifiedIngressProvider>();
    provider->adapter->legacy_fills.emplace_back(
        std::chrono::system_clock::time_point{std::chrono::milliseconds{1'700'000'000'000LL}},
        "BTCUSDT", 1, order_side::buy, 1.0, 100.0);
    auto strategy = std::make_shared<OneShotL2EntryStrategy>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.provider = provider;
    engine eng(std::make_shared<data_handler>(), std::make_shared<orderbook>(),
               strategy, std::move(cfg));

    // The normal L2 path drains latency/pending entries before dispatching
    // the next strategy callback.  The first update parks the strategy
    // entry; the second reaches the actual process_order mutation boundary.
    eng.apply_l2_update("BTCUSDT", tick_side::bid, 100.0, 1);
    eng.apply_l2_update("BTCUSDT", tick_side::bid, 100.0, 1);

    EXPECT_TRUE(strategy->emitted);
    EXPECT_TRUE(eng.is_halted());
    EXPECT_TRUE(provider->adapter->admission_failed);
    EXPECT_EQ(provider->adapter->submits, 0);
}

TEST(EngineStreaming, UnifiedLegacyFillIsRejectedBeforeVenueCancel)
{
    auto provider = std::make_shared<UnifiedIngressProvider>();
    provider->adapter->legacy_fills.emplace_back(
        std::chrono::system_clock::time_point{std::chrono::milliseconds{1'700'000'000'000LL}},
        "BTCUSDT", 1, order_side::buy, 1.0, 100.0);

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.provider = provider;
    engine eng(std::make_shared<data_handler>(), std::make_shared<orderbook>(),
               std::make_shared<StreamTestStrategy>(), std::move(cfg));

    EXPECT_FALSE(eng.cancel_order("BTCUSDT", 1, "test"));
    EXPECT_TRUE(eng.is_halted());
    EXPECT_TRUE(provider->adapter->admission_failed);
    EXPECT_EQ(provider->adapter->cancels, 0);
}

TEST(EngineStreaming, UnifiedLegacyFillIsRejectedBeforeVenueAmend)
{
    auto provider = std::make_shared<UnifiedIngressProvider>();
    provider->adapter->legacy_fills.emplace_back(
        std::chrono::system_clock::time_point{std::chrono::milliseconds{1'700'000'000'000LL}},
        "BTCUSDT", 1, order_side::buy, 1.0, 100.0);

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.provider = provider;
    engine eng(std::make_shared<data_handler>(), std::make_shared<orderbook>(),
               std::make_shared<StreamTestStrategy>(), std::move(cfg));

    EXPECT_FALSE(eng.modify_order("BTCUSDT", 1, 99.0, 1.0));
    EXPECT_TRUE(eng.is_halted());
    EXPECT_TRUE(provider->adapter->admission_failed);
    EXPECT_EQ(provider->adapter->modifies, 0);
}

TEST(EngineStreaming, UnifiedLegacyPollThrowLatchesHaltInsteadOfTerminating)
{
    auto provider = std::make_shared<UnifiedIngressProvider>();
    provider->adapter->throw_on_legacy_poll = true;

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.provider = provider;
    engine eng(std::make_shared<data_handler>(), std::make_shared<orderbook>(),
               std::make_shared<StreamTestStrategy>(), std::move(cfg));

    EXPECT_FALSE(eng.cancel_order("BTCUSDT", 1, "test"));
    EXPECT_TRUE(eng.is_halted());
    EXPECT_TRUE(provider->adapter->admission_failed);
    EXPECT_EQ(provider->adapter->cancels, 0);
}

TEST(EngineStreaming, FinalPrivateEconomicFillIsAccountedThenRequiresReconciliation)
{
    SilenceOutput quiet;
    auto provider = std::make_shared<UnifiedIngressProvider>();
    provider->publish_terminal_on_close = true;
    auto session = std::make_shared<LiveSafetySession>(
        provider, false, std::chrono::milliseconds{100});
    ASSERT_TRUE(session->open_provider());
    auto strategy = std::make_shared<FillCountingStrategy>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.initial_balance = 1'000.0;
    cfg.provider = provider;
    cfg.live_safety_session = session;
    engine eng(std::make_shared<data_handler>(), std::make_shared<orderbook>(),
               strategy, std::move(cfg));

    auto bridge = std::make_shared<DataBridge<bar_record>>(
        std::make_shared<IdleBeforeHeaderTransport>(),
        std::make_shared<CsvBarParser>(), bar_record_sink);
    const auto result = eng.run_streaming(bridge);

    EXPECT_FALSE(result.success());
    EXPECT_TRUE(provider->close_publish_succeeded);
    EXPECT_TRUE(provider->ingress.empty());
    ASSERT_EQ(provider->adapter->acknowledged_terminal_sequences.size(), 1u);
    EXPECT_EQ(provider->adapter->acknowledged_terminal_sequences.front(), 1u);
    EXPECT_TRUE(eng.is_halted());
    EXPECT_EQ(strategy->fills, 0);
    EXPECT_EQ(provider->adapter->submits, 0);
    EXPECT_EQ(provider->adapter->cancels, 0);

    truetest::ui::dashboard_snapshot snapshot;
    ASSERT_TRUE(eng.snapshot_dashboard(snapshot));
    EXPECT_DOUBLE_EQ(snapshot.cash, 900.0);
}

TEST(EngineStreaming, FinalDrainAccountsTrackedFillAfterUnknownLifecycleFault)
{
    SilenceOutput quiet;
    auto provider = std::make_shared<UnifiedIngressProvider>();

    private_execution_record unknown_cancel;
    unknown_cancel.k = private_execution_record::kind::canceled;
    unknown_cancel.event_time_ms = 1'700'000'000'000LL;
    unknown_cancel.side = order_side::sell;
    ASSERT_TRUE(private_execution_record::copy_text(
        unknown_cancel.symbol, unknown_cancel.symbol_size, "BTCUSDT"));
    ASSERT_TRUE(private_execution_record::copy_text(
        unknown_cancel.client_order_id,
        unknown_cancel.client_order_id_size, "foreign"));
    ASSERT_TRUE(private_execution_record::copy_text(
        unknown_cancel.exchange_order_id,
        unknown_cancel.exchange_order_id_size, "foreign-ex"));
    unknown_cancel.cumulative_reported = true;
    unknown_cancel.cumulative_qty = 0.0;

    private_execution_record tracked_fill;
    tracked_fill.k = private_execution_record::kind::full_fill;
    tracked_fill.event_time_ms = 1'700'000'000'001LL;
    tracked_fill.side = order_side::buy;
    tracked_fill.last_fill_qty = 1.0;
    tracked_fill.last_fill_price = 100.0;
    tracked_fill.cumulative_qty = 1.0;
    tracked_fill.cumulative_reported = true;
    ASSERT_TRUE(private_execution_record::copy_text(
        tracked_fill.symbol, tracked_fill.symbol_size, "BTCUSDT"));
    ASSERT_TRUE(private_execution_record::copy_text(
        tracked_fill.client_order_id,
        tracked_fill.client_order_id_size, "tt-1"));
    ASSERT_TRUE(private_execution_record::copy_text(
        tracked_fill.exchange_order_id,
        tracked_fill.exchange_order_id_size, "ex-1"));
    ASSERT_TRUE(private_execution_record::copy_text(
        tracked_fill.execution_id,
        tracked_fill.execution_id_size, "trade-1"));
    provider->close_records = {unknown_cancel, tracked_fill};

    auto session = std::make_shared<LiveSafetySession>(
        provider, false, std::chrono::milliseconds{100});
    ASSERT_TRUE(session->open_provider());
    auto strategy = std::make_shared<FillCountingStrategy>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.initial_balance = 1'000.0;
    cfg.provider = provider;
    cfg.live_safety_session = session;
    engine eng(std::make_shared<data_handler>(), std::make_shared<orderbook>(),
               strategy, std::move(cfg));

    auto bridge = std::make_shared<DataBridge<bar_record>>(
        std::make_shared<IdleBeforeHeaderTransport>(),
        std::make_shared<CsvBarParser>(), bar_record_sink);
    const auto result = eng.run_streaming(bridge);

    EXPECT_FALSE(result.success());
    EXPECT_TRUE(provider->close_records_publish_succeeded);
    EXPECT_TRUE(provider->ingress.empty());
    ASSERT_EQ(provider->adapter->acknowledged_terminal_sequences.size(), 1u);
    EXPECT_EQ(provider->adapter->acknowledged_terminal_sequences.front(), 2u);
    EXPECT_TRUE(provider->adapter->admission_failed);
    EXPECT_TRUE(eng.is_halted());
    EXPECT_EQ(strategy->fills, 0);
    EXPECT_EQ(provider->adapter->submits, 0);
    EXPECT_EQ(provider->adapter->cancels, 0);

    truetest::ui::dashboard_snapshot snapshot;
    ASSERT_TRUE(eng.snapshot_dashboard(snapshot));
    EXPECT_DOUBLE_EQ(snapshot.cash, 900.0);
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
    PrivateLiveLedgerFile ledger_file("operator_stop.bin");
    const std::string& ledger_path = ledger_file.path();
    auto ledger = std::make_shared<AuthoritativeEventLedgerReservation>(
        ledger_path);

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
    cfg.event_log_path = ledger_path;
    cfg.authoritative_event_ledger = ledger;
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
    {
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
    }

    EXPECT_TRUE(ledger->published());
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
