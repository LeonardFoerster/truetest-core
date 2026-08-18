// Pins the live-mode safety gates:
//   1. Reconciler failure blocks engine construction.
//   2. Noop default passes (engine constructs).
//   3. Kill-switch fires during stop_workers when mode == live.
// None of these touch the network - they use in-memory mocks.

#include <gtest/gtest.h>
#include "core/event_log.h"
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "engine/live_safety_session.h"
#include "data/data_handler.h"
#include "exits/default_exit_policy.h"
#include "orderbook/orderbook.h"
#include "market_maker/market_maker.h"
#include "strategy/strategy_interface.h"
#include "execution/live_safety.h"
#include "providers/provider.h"
#include "helpers/alloc_counter.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unordered_map>
#include <unistd.h>
#include <vector>

TEST(ProviderFundingIngress, FifoValueHandoffIsAllocationFree)
{
    ProviderFundingIngress ingress;
    const auto base = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'700'000'000'000LL}};

    bool exact = true;
    truetest::test::alloc::snapshot allocations;
    {
        truetest::test::alloc::measure_window window;
        for (int i = 0; i < 1'000; ++i)
        {
            provider_funding_update update;
            exact = exact && ingress.try_publish(
                base + std::chrono::milliseconds{i}, "BTCUSDT", i + 0.5);
            exact = exact && ingress.try_pop(update);
            exact = exact
                && update.event_time_ms == 1'700'000'000'000LL + i
                && update.symbol_view() == "BTCUSDT"
                && update.cash_delta == i + 0.5;
        }
        allocations = window.total();
    }
    EXPECT_TRUE(exact);
    EXPECT_EQ(allocations.count, 0u);
    EXPECT_EQ(allocations.bytes, 0u);
    EXPECT_FALSE(ingress.failed());
}

TEST(ProviderFundingIngress, CapacityPlusOneLatchesWithoutOverwrite)
{
    ProviderFundingIngress ingress;
    const auto ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'700'000'000'000LL}};
    for (std::size_t i = 0; i < ProviderFundingIngress::capacity; ++i)
        ASSERT_TRUE(ingress.try_publish(
            ts, "BTCUSDT", static_cast<double>(i) + 1.0));
    EXPECT_FALSE(ingress.try_publish(ts, "BTCUSDT", 65.0));
    EXPECT_TRUE(ingress.failed());

    provider_funding_update update;
    for (std::size_t i = 0; i < ProviderFundingIngress::capacity; ++i)
    {
        ASSERT_TRUE(ingress.try_pop(update));
        EXPECT_DOUBLE_EQ(update.cash_delta, static_cast<double>(i) + 1.0);
    }
    EXPECT_FALSE(ingress.try_pop(update));
    EXPECT_FALSE(ingress.try_publish(ts, "BTCUSDT", 66.0));
}

TEST(ProviderFundingIngress, InvalidValueLatchesFailure)
{
    ProviderFundingIngress ingress;
    const auto ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'700'000'000'000LL}};
    EXPECT_FALSE(ingress.try_publish(
        ts, "BTCUSDT", std::numeric_limits<double>::quiet_NaN()));
    EXPECT_TRUE(ingress.failed());
}

TEST(ProviderFundingIngress, FullLengthFundingEventConstructionIsAllocationFree)
{
    constexpr std::string_view symbol{"ABCDEFGHIJKLMNOPQRSTUVWXYZ12345"};
    static_assert(symbol.size() == provider_funding_update::symbol_capacity);
    const auto ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'700'000'000'000LL}};

    bool exact = false;
    truetest::test::alloc::snapshot allocations;
    {
        truetest::test::alloc::measure_window window;
        funding_event event(ts, symbol, 0.0, -1.25, "FUNDING_FEE");
        exact = event.get_symbol() == symbol
             && event.get_reason() == "FUNDING_FEE"
             && event.get_cash_delta() == -1.25;
        allocations = window.total();
    }
    EXPECT_TRUE(exact);
    EXPECT_EQ(allocations.count, 0u);
    EXPECT_EQ(allocations.bytes, 0u);
}

namespace {

class LiveLedgerFile
{
public:
    explicit LiveLedgerFile(std::string_view label)
    {
        std::string pattern = "/tmp/truetest_live_ledger_XXXXXX";
        char* created = ::mkdtemp(pattern.data());
        if (created == nullptr)
            throw std::runtime_error(
                "cannot create private live ledger test directory");
        directory_ = created;
        if (::chmod(directory_.c_str(), S_IRWXU) != 0)
        {
            (void)::rmdir(directory_.c_str());
            throw std::runtime_error(
                "cannot secure live ledger test directory");
        }
        path_ = directory_ + "/" + std::string(label) + ".bin";
    }

    ~LiveLedgerFile()
    {
        (void)::unlink((path_ + ".partial").c_str());
        (void)::unlink(path_.c_str());
        (void)::rmdir(directory_.c_str());
    }

    void apply(engine_config& cfg)
    {
        cfg.event_log_path = path_;
        cfg.authoritative_event_ledger =
            std::make_shared<AuthoritativeEventLedgerReservation>(path_);
    }

    const std::string& path() const noexcept { return path_; }

private:
    std::string directory_;
    std::string path_;
};

private_execution_record make_private_record(
    private_execution_record::kind k,
    std::int64_t event_time_ms = 1'700'000'000'000LL)
{
    private_execution_record record;
    record.k = k;
    record.event_time_ms = event_time_ms;
    record.engine_order_id = 42;
    record.side = order_side::buy;
    EXPECT_TRUE(private_execution_record::copy_text(
        record.symbol, record.symbol_size, "BTCUSDT"));
    EXPECT_TRUE(private_execution_record::copy_text(
        record.client_order_id, record.client_order_id_size, "tt-42"));
    EXPECT_TRUE(private_execution_record::copy_text(
        record.exchange_order_id, record.exchange_order_id_size, "venue-42"));
    record.cumulative_reported = true;
    if (k == private_execution_record::kind::partial_fill
        || k == private_execution_record::kind::full_fill)
    {
        record.last_fill_qty = 1.0;
        record.last_fill_price = 100.0;
        record.cumulative_qty = 1.0;
        record.remaining_qty = (k == private_execution_record::kind::partial_fill)
            ? 1.0 : 0.0;
        EXPECT_TRUE(private_execution_record::copy_text(
            record.execution_id, record.execution_id_size, "exec-42"));
    }
    return record;
}

} // namespace

TEST(ProviderExecutionIngress, UnifiedPrivateRecordsRemainFifoAndAllocationFree)
{
    ProviderExecutionIngress ingress;
    auto ack = make_private_record(private_execution_record::kind::ack);
    auto fill = make_private_record(private_execution_record::kind::partial_fill,
                                    1'700'000'000'001LL);

    bool exact = true;
    truetest::test::alloc::snapshot allocations;
    {
        truetest::test::alloc::measure_window window;
        exact = ingress.try_publish(ack) && ingress.try_publish(fill);
        private_execution_record popped;
        exact = exact && ingress.try_pop(popped)
            && popped.sequence == 1
            && popped.k == private_execution_record::kind::ack
            && popped.symbol_view() == "BTCUSDT";
        exact = exact && ingress.try_pop(popped)
            && popped.sequence == 2
            && popped.k == private_execution_record::kind::partial_fill
            && popped.execution_id_view() == "exec-42";
        allocations = window.total();
    }

    EXPECT_TRUE(exact);
    EXPECT_EQ(allocations.count, 0u);
    EXPECT_EQ(allocations.bytes, 0u);
    EXPECT_FALSE(ingress.failed());
}

TEST(ProviderExecutionIngress, OverflowLatchesWithoutOverwritingAdmittedRecords)
{
    ProviderExecutionIngress ingress;
    for (std::size_t i = 0; i < ProviderExecutionIngress::capacity; ++i)
    {
        auto record = make_private_record(private_execution_record::kind::ack,
                                          1'700'000'000'000LL
                                              + static_cast<std::int64_t>(i));
        ASSERT_TRUE(ingress.try_publish(record));
    }

    auto overflow = make_private_record(private_execution_record::kind::ack,
                                        1'700'000'000'999LL);
    EXPECT_FALSE(ingress.try_publish(overflow));
    EXPECT_TRUE(ingress.failed());

    private_execution_record popped;
    for (std::size_t i = 0; i < ProviderExecutionIngress::capacity; ++i)
    {
        ASSERT_TRUE(ingress.try_pop(popped));
        EXPECT_EQ(popped.sequence, i + 1);
        EXPECT_EQ(popped.event_time_ms,
                  1'700'000'000'000LL + static_cast<std::int64_t>(i));
    }
    EXPECT_FALSE(ingress.try_pop(popped));
}

TEST(ProviderExecutionIngress, RejectsAContradictoryNonFillEconomicIncrement)
{
    ProviderExecutionIngress ingress;
    auto canceled = make_private_record(private_execution_record::kind::canceled);
    canceled.last_fill_qty = 1.0;
    canceled.last_fill_price = 100.0;
    EXPECT_FALSE(ingress.try_publish(canceled));
    EXPECT_TRUE(ingress.failed());
}

TEST(ProviderExecutionIngress, RejectsTerminalLifecycleWithoutCumulativeProof)
{
    ProviderExecutionIngress ingress;
    auto canceled = make_private_record(private_execution_record::kind::canceled);
    canceled.cumulative_reported = false;

    EXPECT_FALSE(ingress.try_publish(canceled));
    EXPECT_TRUE(ingress.failed());
}

namespace {

class NullStrategy : public IStrategy
{
public:
    std::optional<order_event> on_market(const market_event&) override { return std::nullopt; }
    void set_position_open(const std::string&, bool) override {}
};

class FailingReconciler : public IReconciler
{
public:
    std::string reconcile(const portfolio&, double) override
    {
        return "balance drift > tolerance: local=100, exchange=50";
    }
};

class SpyKillSwitch : public IKillSwitch
{
public:
    std::atomic<int> invocations{0};
    std::atomic<long> last_deadline_ms{0};
    bool cancel_all_and_flatten(std::chrono::milliseconds d) override
    {
        invocations.fetch_add(1);
        last_deadline_ms.store(d.count());
        return true;
    }
};

class RecoveryBracketAdapter final
    : public truetest::exits::IBracketAdapter
{
public:
    explicit RecoveryBracketAdapter(
        std::vector<recovered_bracket> recovered)
        : recovered_(std::move(recovered)) {}

    truetest::exits::bracket_caps capabilities() const override
    {
        return {.stop_market = true};
    }

    truetest::exits::bracket_handles place(
        std::uint64_t, const truetest::exits::exit_intent&, double) override
    {
        return {};
    }

    void cancel(std::uint64_t,
                const truetest::exits::bracket_handles&) override {}

    std::vector<recovered_bracket> list_open() override
    {
        return recovered_;
    }

private:
    std::vector<recovered_bracket> recovered_;
};

class RecoveryProvider final : public IProvider
{
public:
    explicit RecoveryProvider(
        std::shared_ptr<RecoveryBracketAdapter> bracket)
        : bracket_(std::move(bracket)) {}

    std::string name() const override { return "recovery-test"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return true; }
    bool open() override { state_ = lifecycle::open; return true; }
    void close() override { state_ = lifecycle::closed; }
    lifecycle lifecycle_state() const override { return state_; }
    std::shared_ptr<IDataTransport> get_transport() override { return {}; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return {};
    }
    std::shared_ptr<IKillSwitch> get_kill_switch() override { return kill_; }
    std::shared_ptr<truetest::exits::IBracketAdapter>
    get_bracket_adapter() override { return bracket_; }

private:
    lifecycle state_ = lifecycle::closed;
    std::shared_ptr<RecoveryBracketAdapter> bracket_;
    std::shared_ptr<SpyKillSwitch> kill_ = std::make_shared<SpyKillSwitch>();
};

// The engine deliberately passes a const portfolio to production
// reconcilers. This test double seeds the already-reconciled local snapshot so
// the constructor's bracket/position consistency gate can be exercised in
// isolation; it is not a production reconciliation pattern.
class PositionSeedingReconciler final : public IReconciler
{
public:
    explicit PositionSeedingReconciler(
        std::unordered_map<std::string, position> positions)
        : positions_(std::move(positions)) {}

    std::string reconcile(const portfolio& current, double) override
    {
        const_cast<portfolio&>(current).restore_state(
            current.get_cash(), current.get_total_trades(), positions_);
        return {};
    }

private:
    std::unordered_map<std::string, position> positions_;
};

truetest::exits::IBracketAdapter::recovered_bracket recovered_bracket(
    std::uint64_t opener, double qty, order_side close_side)
{
    truetest::exits::IBracketAdapter::recovered_bracket rb;
    rb.opener_order_id = opener;
    rb.symbol = "BTCUSDT";
    rb.close_side = close_side;
    rb.qty = qty;
    rb.entry_price = 100.0;
    rb.stop_loss = 90.0;
    rb.take_profit = 110.0;
    rb.handles.sl_exchange_id = std::to_string(opener * 10 + 1);
    rb.handles.tp_exchange_id = std::to_string(opener * 10 + 2);
    rb.handles.symbol = rb.symbol;
    return rb;
}

std::unique_ptr<engine> construct_recovery_engine(
    std::vector<truetest::exits::IBracketAdapter::recovered_bracket> recovered,
    std::unordered_map<std::string, position> positions,
    LiveLedgerFile& ledger)
{
    auto adapter = std::make_shared<RecoveryBracketAdapter>(
        std::move(recovered));
    auto provider = std::make_shared<RecoveryProvider>(std::move(adapter));
    auto session = std::make_shared<LiveSafetySession>(
        provider, true, std::chrono::milliseconds{50});
    if (!session->open_provider())
        throw std::runtime_error("test provider failed to open");

    engine_config cfg;
    cfg.mode = engine_mode::live;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    ledger.apply(cfg);
    cfg.provider = provider;
    cfg.live_safety_session = session;
    cfg.reconciler = std::make_shared<PositionSeedingReconciler>(
        std::move(positions));

    return std::make_unique<engine>(
        std::make_shared<data_handler>(), std::make_shared<orderbook>(),
        std::make_shared<NullStrategy>(), std::move(cfg));
}

class CallbackDms
{
public:
    void set_failure_callback(std::function<void(std::string_view)> callback)
    {
        callback_ = std::move(callback);
    }

    void fail(std::string_view reason) { callback_(reason); }

private:
    std::function<void(std::string_view)> callback_;
};

class StopRecordingTransport final : public IDataTransport
{
public:
    explicit StopRecordingTransport(std::vector<std::string>& events)
        : events_(events) {}

    bool open() override { return true; }
    void close() override {}
    bool is_open() const override { return true; }
    std::optional<std::string> read_line() override { return std::nullopt; }
    void request_stop() override { events_.push_back("stop"); }

private:
    std::vector<std::string>& events_;
};

class ShutdownResourceSpy
{
public:
    ShutdownResourceSpy(std::vector<std::string>& events, std::string tag)
        : events_(events), tag_(std::move(tag)) {}

    void request_stop() { events_.push_back(tag_ + ":request-stop"); }
    void quiesce() { events_.push_back(tag_ + ":quiesce"); }
    void stop() { events_.push_back(tag_ + ":stop"); }
    void close()
    {
        events_.push_back(tag_ + ":close");
        if (throw_on_close) throw std::runtime_error("close failed");
    }
    bool disarm()
    {
        events_.push_back(tag_ + ":disarm");
        return disarm_result;
    }

    bool disarm_result = true;
    bool throw_on_close = false;

private:
    std::vector<std::string>& events_;
    std::string tag_;
};

}

TEST(LiveSafety, ReconcilerFailure_BlocksConstruction)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();

    LiveLedgerFile ledger("reconciler_failure");
    engine_config cfg;
    cfg.mode = engine_mode::live;
    ledger.apply(cfg);
    cfg.reconciler = std::make_shared<FailingReconciler>();

    EXPECT_THROW(engine eng(dh, ob, strat, std::move(cfg)), std::runtime_error);
}

TEST(LiveSafety, MissingReconciler_BlocksConstruction)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();

    LiveLedgerFile ledger("missing_reconciler");
    engine_config cfg;
    cfg.mode = engine_mode::live;
    ledger.apply(cfg);
    EXPECT_THROW(engine eng(dh, ob, strat, std::move(cfg)),
                 std::runtime_error);
}

TEST(LiveSafety, LiveEngineRequiresPreReservedAuthoritativeLedger)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();

    engine_config cfg;
    cfg.mode = engine_mode::live;
    cfg.reconciler = std::make_shared<FailingReconciler>();

    try
    {
        engine eng(dh, ob, strat, std::move(cfg));
        FAIL() << "live construction accepted an unreserved event ledger";
    }
    catch (const std::runtime_error& e)
    {
        EXPECT_NE(std::string(e.what()).find("pre-reserved authoritative event ledger"),
                  std::string::npos);
    }
}

TEST(LiveSafety, LiveEngineRefusesAuthoritativeLedgerRotation)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();
    LiveLedgerFile ledger("rotation_refused");

    engine_config cfg;
    cfg.mode = engine_mode::live;
    ledger.apply(cfg);
    cfg.log_max_bytes = 1024;
    cfg.reconciler = std::make_shared<FailingReconciler>();

    try
    {
        engine eng(dh, ob, strat, std::move(cfg));
        FAIL() << "live construction accepted authoritative ledger rotation";
    }
    catch (const std::runtime_error& e)
    {
        EXPECT_NE(std::string(e.what()).find("does not permit rotation"),
                  std::string::npos);
    }
}

TEST(LiveSafety, BacktestMode_SkipsReconcileGate)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    // Even a failing reconciler must NOT block backtest startup.
    cfg.reconciler = std::make_shared<FailingReconciler>();

    EXPECT_NO_THROW(engine eng(dh, ob, strat, std::move(cfg)));
}

TEST(LiveSafety, RecoverySingleMissingQuantityUsesReconciledPosition)
{
    LiveLedgerFile ledger("recovery_single_missing_qty");
    EXPECT_NO_THROW({
        auto eng = construct_recovery_engine(
            {recovered_bracket(7, 0.0, order_side::sell)},
            {{"BTCUSDT", position{.qty = 2.0, .cost_basis = 100.0}}}, ledger);
    });
}

TEST(LiveSafety, RecoveryWithoutReconciledPositionRefusesStartup)
{
    LiveLedgerFile ledger("recovery_no_position");
    EXPECT_THROW(
        construct_recovery_engine(
            {recovered_bracket(7, 1.0, order_side::sell)}, {}, ledger),
        std::runtime_error);
}

TEST(LiveSafety, RecoveryWrongCloseSideRefusesStartup)
{
    LiveLedgerFile ledger("recovery_wrong_side");
    EXPECT_THROW(
        construct_recovery_engine(
            {recovered_bracket(7, 2.0, order_side::buy)},
            {{"BTCUSDT", position{.qty = 2.0, .cost_basis = 100.0}}}, ledger),
        std::runtime_error);
}

TEST(LiveSafety, RecoveryMultipleMissingQuantitiesRefuseStartup)
{
    LiveLedgerFile ledger("recovery_multiple_missing");
    EXPECT_THROW(
        construct_recovery_engine(
            {recovered_bracket(7, 0.0, order_side::sell),
             recovered_bracket(8, 0.0, order_side::sell)},
            {{"BTCUSDT", position{.qty = 2.0, .cost_basis = 100.0}}}, ledger),
        std::runtime_error);
}

TEST(LiveSafety, RecoverySingleQuantityMismatchRefusesStartup)
{
    LiveLedgerFile ledger("recovery_quantity_mismatch");
    EXPECT_THROW(
        construct_recovery_engine(
            {recovered_bracket(7, 1.5, order_side::sell)},
            {{"BTCUSDT", position{.qty = 2.0, .cost_basis = 100.0}}}, ledger),
        std::runtime_error);
}

TEST(LiveSafety, RecoveryAggregateExactQuantityConstructs)
{
    LiveLedgerFile ledger("recovery_aggregate_exact");
    EXPECT_NO_THROW({
        auto eng = construct_recovery_engine(
            {recovered_bracket(7, 0.75, order_side::sell),
             recovered_bracket(8, 1.25, order_side::sell)},
            {{"BTCUSDT", position{.qty = 2.0, .cost_basis = 100.0}}}, ledger);
    });
}

TEST(LiveSafety, RecoveryAggregateOverPositionRefusesStartup)
{
    LiveLedgerFile ledger("recovery_aggregate_over");
    EXPECT_THROW(
        construct_recovery_engine(
            {recovered_bracket(7, 1.0, order_side::sell),
             recovered_bracket(8, 1.25, order_side::sell)},
            {{"BTCUSDT", position{.qty = 2.0, .cost_basis = 100.0}}}, ledger),
        std::runtime_error);
}

TEST(LiveSafety, DmsFailureWiringHaltsBeforeWakingStream)
{
    auto dms = std::make_shared<CallbackDms>();
    std::vector<std::string> events;
    auto transport = std::make_shared<StopRecordingTransport>(events);

    wire_dms_failure_to_engine(
        dms,
        [&events](std::string_view reason) {
            events.emplace_back("halt:" + std::string(reason));
        },
        transport);
    dms->fail("heartbeat failed");

    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0], "halt:heartbeat failed");
    EXPECT_EQ(events[1], "stop");
}

TEST(LiveSafety, FuturesProviderResourcesDisarmOnlyAfterOrderedFinish)
{
    std::vector<std::string> events;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto dms = std::make_shared<ShutdownResourceSpy>(events, "dms");
    auto bridge = std::make_shared<ShutdownResourceSpy>(events, "bridge");
    auto private_transport =
        std::make_shared<ShutdownResourceSpy>(events, "private");
    auto public_transport =
        std::make_shared<ShutdownResourceSpy>(events, "public");

    quiesce_futures_live_resources(
        cancelled, dms, bridge, private_transport, public_transport);
    EXPECT_TRUE(cancelled->load(std::memory_order_acquire));
    EXPECT_TRUE(finish_futures_live_resources(
        dms, bridge, private_transport, public_transport,
        live_shutdown_disposition::disarm_after_kill));

    EXPECT_EQ(events, (std::vector<std::string>{
        "dms:request-stop", "bridge:quiesce", "public:request-stop",
        "dms:stop", "bridge:close", "private:close", "public:close",
        "dms:disarm"}));
}

TEST(LiveSafety, FuturesProviderResourcesPreserveDmsAfterAmbiguousKill)
{
    std::vector<std::string> events;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto dms = std::make_shared<ShutdownResourceSpy>(events, "dms");
    auto bridge = std::make_shared<ShutdownResourceSpy>(events, "bridge");
    auto private_transport =
        std::make_shared<ShutdownResourceSpy>(events, "private");
    auto public_transport =
        std::make_shared<ShutdownResourceSpy>(events, "public");

    quiesce_futures_live_resources(
        cancelled, dms, bridge, private_transport, public_transport);
    EXPECT_TRUE(finish_futures_live_resources(
        dms, bridge, private_transport, public_transport,
        live_shutdown_disposition::preserve_dead_man_switch));

    EXPECT_EQ(std::find(events.begin(), events.end(), "dms:disarm"),
              events.end());
}

TEST(LiveSafety, FuturesProviderResourcesReportDisarmFailure)
{
    std::vector<std::string> events;
    auto dms = std::make_shared<ShutdownResourceSpy>(events, "dms");
    auto bridge = std::make_shared<ShutdownResourceSpy>(events, "bridge");
    auto private_transport =
        std::make_shared<ShutdownResourceSpy>(events, "private");
    auto public_transport =
        std::make_shared<ShutdownResourceSpy>(events, "public");
    dms->disarm_result = false;

    EXPECT_FALSE(finish_futures_live_resources(
        dms, bridge, private_transport, public_transport,
        live_shutdown_disposition::disarm_after_kill));
    EXPECT_EQ(events.back(), "dms:disarm");
}

TEST(LiveSafety, FuturesProviderResourcesContinueClosingAndPreserveDmsOnFailure)
{
    std::vector<std::string> events;
    auto dms = std::make_shared<ShutdownResourceSpy>(events, "dms");
    auto bridge = std::make_shared<ShutdownResourceSpy>(events, "bridge");
    auto private_transport =
        std::make_shared<ShutdownResourceSpy>(events, "private");
    auto public_transport =
        std::make_shared<ShutdownResourceSpy>(events, "public");
    bridge->throw_on_close = true;

    EXPECT_FALSE(finish_futures_live_resources(
        dms, bridge, private_transport, public_transport,
        live_shutdown_disposition::disarm_after_kill));
    EXPECT_NE(std::find(events.begin(), events.end(), "private:close"),
              events.end());
    EXPECT_NE(std::find(events.begin(), events.end(), "public:close"),
              events.end());
    EXPECT_EQ(std::find(events.begin(), events.end(), "dms:disarm"),
              events.end());
}

// ---------------------------------------------------------------------------
// Direct behavioral test for provider_callbacks_armed_ guard (post-stop safety)
// The engine disarms the atomic *before* closing transports so that any
// in-flight callbacks from provider threads see the guard and early-return.
// ---------------------------------------------------------------------------

namespace {

// Minimal spy provider that captures the remaining engine halt callback so we
// can invoke it after construction/teardown and verify the armed guard.
class ArmedGuardSpyProvider : public IProvider
{
public:
    std::function<void(std::string_view)> captured_halt;
    bool throw_after_storing_halt = false;

    std::string name() const override { return "armed-spy"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return true; }

    bool open() override { return true; }
    void close() override {}

    std::shared_ptr<IDataTransport> get_transport() override { return nullptr; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override { return nullptr; }

    void set_halt_callback(std::function<void(std::string_view reason)> cb) override
    {
        captured_halt = std::move(cb);
        if (throw_after_storing_halt)
            throw std::runtime_error("halt callback registration failed");
    }
};

} // anonymous

namespace {

class ShutdownOrderKillSwitch final : public IKillSwitch
{
public:
    ShutdownOrderKillSwitch(std::vector<std::string>& events, std::mutex& mu)
        : events_(events), mu_(mu) {}

    bool cancel_all_and_flatten(std::chrono::milliseconds) override
    {
        ++calls;
        std::lock_guard<std::mutex> lock(mu_);
        events_.emplace_back("kill");
        return true;
    }

    std::atomic<int> calls{0};

private:
    std::vector<std::string>& events_;
    std::mutex& mu_;
};

class ShutdownOrderProvider final : public IProvider
{
public:
    ShutdownOrderProvider()
        : kill(std::make_shared<ShutdownOrderKillSwitch>(events, events_mu)) {}

    std::string name() const override { return "shutdown-order-spy"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return true; }
    bool open() override { state = lifecycle::open; return true; }
    void close() override { record("legacy-close"); state = lifecycle::closed; }
    lifecycle lifecycle_state() const override { return state; }
    std::shared_ptr<IDataTransport> get_transport() override { return {}; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override { return {}; }
    std::shared_ptr<IKillSwitch> get_kill_switch() override { return kill; }
    void set_halt_callback(std::function<void(std::string_view reason)> cb) override
    {
        halt_callback = std::move(cb);
    }
    void quiesce_for_live_shutdown() override { record("quiesce"); }
    void finish_live_shutdown(live_shutdown_disposition d) override
    {
        disposition = d;
        record("finish");
        if (throw_on_finish)
            throw std::runtime_error("finish failure");
        state = lifecycle::closed;
    }
    bool private_execution_producer_joined() const noexcept override
    {
        // This fixture has no private reader; finish_live_shutdown is its
        // synchronous producer barrier.
        return state == lifecycle::closed;
    }

    std::shared_ptr<ShutdownOrderKillSwitch> kill;
    live_shutdown_disposition disposition =
        live_shutdown_disposition::preserve_dead_man_switch;
    std::vector<std::string> events;
    std::mutex events_mu;
    std::function<void(std::string_view)> halt_callback;
    bool throw_on_finish = false;

private:
    void record(const char* value)
    {
        std::lock_guard<std::mutex> lock(events_mu);
        events.emplace_back(value);
    }

    lifecycle state = lifecycle::closed;
};

} // namespace

TEST(LiveSafety, ProviderHaltCallbackArmedGuardPreventsLateInvocation)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();

    auto spy = std::make_shared<ArmedGuardSpyProvider>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;   // avoid live gates
    cfg.provider = spy;

    // Construction wires the guarded callbacks into the provider.
    {
        engine eng(dh, ob, strat, std::move(cfg));
        // eng dtor will disarm (in ~engine / stop path)
    }

    // Invoke the captured callback after the engine and its armed flag are
    // gone. It must early-return safely.
    if (spy->captured_halt)
    {
        spy->captured_halt("late callback after stop");
    }

    SUCCEED();
}

TEST(LiveSafety, LiveEngineRefusesSessionThatNeverOwnedProviderOpen)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();
    auto spy = std::make_shared<ArmedGuardSpyProvider>();

    engine_config cfg;
    cfg.mode = engine_mode::live;
    cfg.provider = spy;
    cfg.live_safety_session = std::make_shared<LiveSafetySession>(
        spy, true, std::chrono::milliseconds{50});
    EXPECT_THROW(engine eng(dh, ob, strat, std::move(cfg)),
                 std::runtime_error);
}

TEST(LiveSafety, LiveEngineDestructorUsesSessionShutdownExactlyOnce)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();
    auto provider = std::make_shared<ShutdownOrderProvider>();
    auto session = std::make_shared<LiveSafetySession>(
        provider, true, std::chrono::milliseconds{50});
    ASSERT_TRUE(session->open_provider());
    LiveLedgerFile ledger("destructor_shutdown");

    {
        engine_config cfg;
        cfg.mode = engine_mode::live;
        cfg.threading = thread_preset::inline_mode;
        cfg.disable_pinning = true;
        ledger.apply(cfg);
        cfg.provider = provider;
        cfg.live_safety_session = session;
        cfg.reconciler = std::make_shared<NoopReconciler>();
        engine eng(dh, ob, strat, std::move(cfg));
    }

    EXPECT_EQ(provider->kill->calls.load(), 1);
    EXPECT_EQ(provider->disposition,
              live_shutdown_disposition::disarm_after_kill);
    ASSERT_EQ(provider->events.size(), 3u);
    EXPECT_EQ(provider->events[0], "quiesce");
    EXPECT_EQ(provider->events[1], "kill");
    EXPECT_EQ(provider->events[2], "finish");

    EXPECT_TRUE(std::ifstream(ledger.path(), std::ios::binary).good());
    EXPECT_FALSE(std::ifstream(ledger.path() + ".partial",
                               std::ios::binary).good());

    const auto report = session->shutdown_once(live_shutdown_reason::normal_end);
    EXPECT_TRUE(report.kill_succeeded);
    EXPECT_EQ(provider->kill->calls.load(), 1);
    EXPECT_EQ(provider->events.size(), 3u);
}

TEST(LiveSafety, ProviderFatalCallbackHaltsThenUsesSharedShutdownSession)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();
    auto provider = std::make_shared<ShutdownOrderProvider>();
    auto session = std::make_shared<LiveSafetySession>(
        provider, true, std::chrono::milliseconds{50});
    ASSERT_TRUE(session->open_provider());
    LiveLedgerFile ledger("fatal_callback");

    {
        engine_config cfg;
        cfg.mode = engine_mode::live;
        cfg.threading = thread_preset::inline_mode;
        cfg.disable_pinning = true;
        ledger.apply(cfg);
        cfg.provider = provider;
        cfg.live_safety_session = session;
        cfg.reconciler = std::make_shared<NoopReconciler>();
        engine eng(dh, ob, strat, std::move(cfg));
        ASSERT_TRUE(provider->halt_callback);
        provider->halt_callback("simulated DMS heartbeat failure");
        EXPECT_TRUE(eng.is_halted());
        EXPECT_FALSE(eng.run_succeeded());
    }

    ASSERT_EQ(provider->events.size(), 3u);
    EXPECT_EQ(provider->events[0], "quiesce");
    EXPECT_EQ(provider->events[1], "kill");
    EXPECT_EQ(provider->events[2], "finish");
    EXPECT_EQ(provider->kill->calls.load(), 1);
}

TEST(LiveSafety, FinishFailureCannotReportSuccessfulLiveRun)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();
    auto provider = std::make_shared<ShutdownOrderProvider>();
    auto session = std::make_shared<LiveSafetySession>(
        provider, true, std::chrono::milliseconds{50});
    ASSERT_TRUE(session->open_provider());
    LiveLedgerFile ledger("finish_failure");

    {
        engine_config cfg;
        cfg.mode = engine_mode::live;
        cfg.threading = thread_preset::inline_mode;
        cfg.disable_pinning = true;
        ledger.apply(cfg);
        cfg.provider = provider;
        cfg.live_safety_session = session;
        cfg.reconciler = std::make_shared<NoopReconciler>();
        engine eng(dh, ob, strat, std::move(cfg));

        provider->throw_on_finish = true;
        EXPECT_FALSE(eng.finalize_live_shutdown(
            live_shutdown_reason::normal_end));
        EXPECT_FALSE(eng.run_succeeded());
    }

    EXPECT_EQ(provider->kill->calls.load(), 1);
    ASSERT_EQ(provider->events.size(), 3u);
    EXPECT_EQ(provider->events[0], "quiesce");
    EXPECT_EQ(provider->events[1], "kill");
    EXPECT_EQ(provider->events[2], "finish");
    EXPECT_FALSE(std::ifstream(ledger.path(), std::ios::binary).good());
    EXPECT_TRUE(std::ifstream(ledger.path() + ".partial",
                               std::ios::binary).good());
}

TEST(LiveSafety, OperatorKillReportsFailureWhenProviderCannotFinishClosing)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();
    auto provider = std::make_shared<ShutdownOrderProvider>();
    auto session = std::make_shared<LiveSafetySession>(
        provider, true, std::chrono::milliseconds{50});
    ASSERT_TRUE(session->open_provider());
    LiveLedgerFile ledger("operator_kill_failure");

    {
        engine_config cfg;
        cfg.mode = engine_mode::live;
        cfg.threading = thread_preset::inline_mode;
        cfg.disable_pinning = true;
        ledger.apply(cfg);
        cfg.provider = provider;
        cfg.live_safety_session = session;
        cfg.reconciler = std::make_shared<NoopReconciler>();
        engine eng(dh, ob, strat, std::move(cfg));

        provider->throw_on_finish = true;
        EXPECT_FALSE(eng.request_operator_kill(std::chrono::milliseconds{50}));
        EXPECT_TRUE(eng.is_halted());
        EXPECT_FALSE(eng.run_succeeded());
    }

    EXPECT_EQ(provider->kill->calls.load(), 1);
    ASSERT_EQ(provider->events.size(), 3u);
    EXPECT_EQ(provider->events[0], "quiesce");
    EXPECT_EQ(provider->events[1], "kill");
    EXPECT_EQ(provider->events[2], "finish");
}

TEST(LiveSafety, ThrowingConstructorLeavesCapturedCallbacksDisarmed)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();
    auto spy = std::make_shared<ArmedGuardSpyProvider>();
    auto session = std::make_shared<LiveSafetySession>(
        spy, true, std::chrono::milliseconds{50});
    ASSERT_TRUE(session->open_provider());
    LiveLedgerFile ledger("constructor_callback_disarm");

    engine_config cfg;
    cfg.mode = engine_mode::live;
    ledger.apply(cfg);
    cfg.provider = spy;
    cfg.live_safety_session = session;
    cfg.reconciler = std::make_shared<FailingReconciler>();
    EXPECT_THROW(engine eng(dh, ob, strat, std::move(cfg)),
                 std::runtime_error);

    EXPECT_FALSE(static_cast<bool>(spy->captured_halt));
}

TEST(LiveSafety, ThrowingHaltRegistrationDisarmsEveryCapturedCallback)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();
    auto spy = std::make_shared<ArmedGuardSpyProvider>();
    auto session = std::make_shared<LiveSafetySession>(
        spy, true, std::chrono::milliseconds{50});
    ASSERT_TRUE(session->open_provider());
    spy->throw_after_storing_halt = true;
    LiveLedgerFile ledger("halt_registration_disarm");

    engine_config cfg;
    cfg.mode = engine_mode::live;
    ledger.apply(cfg);
    cfg.provider = spy;
    cfg.live_safety_session = session;
    cfg.reconciler = std::make_shared<NoopReconciler>();
    EXPECT_THROW(engine eng(dh, ob, strat, std::move(cfg)),
                 std::runtime_error);

    ASSERT_TRUE(spy->captured_halt);
    EXPECT_NO_THROW(spy->captured_halt("late halt"));
}

// ---------------------------------------------------------------------------
// Risk halt must raise the process-wide halt_flag_ (S3 terminal), not only a
// local halt_requested out-param. DataBridge / L2 dispatch / streaming loops
// all observe halt_flag_; without this, live can keep submitting after a risk
// halt decision.
// ---------------------------------------------------------------------------

namespace {

// Buys a fixed qty every bar until the engine stops calling us.
class RelentlessBuyer : public IStrategy
{
    double qty_;
    int calls_ = 0;
public:
    explicit RelentlessBuyer(double qty = 10.0) : qty_(qty) {}
    int calls() const { return calls_; }
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        ++calls_;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy,
                           qty_, mkt.get_close());
    }
    void set_position_open(const std::string&, bool) override {}
};

} // anonymous

TEST(LiveSafety, RiskHaltSetsProcessWideHaltFlag)
{
    // Declining market + aggressive buyer + tight max_drawdown → risk halt.
    auto dh = std::make_shared<data_handler>();
    double price = 100.0;
    for (int i = 0; i < 200; ++i)
    {
        const double close = price * (1.0 - 0.01);
        dh->load_into_queue("2024-01-01", "TEST",
                            price, price + 0.1, close - 0.1, close, 1000);
        price = close;
    }

    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<RelentlessBuyer>(10.0);

    // Liquidity so market buys fill and equity can draw down.
    MarketMaker mm(3);
    mm.add_orders(ob, 100.0, 100);

    engine_config cfg;
    cfg.mode            = engine_mode::backtest;
    cfg.initial_balance = 10000.0;
    cfg.seed            = 7;
    cfg.threading       = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    // Live-safety path under test: hard portfolio halt (not soft research mode).
    cfg.risk_soft_portfolio_limits = false;
    // No platform SL/TP — otherwise stops flatten inventory before DD trips.
    cfg.exit_defaults.mode   = truetest::exits::exit_policy_mode::strategy_only;
    cfg.exit_defaults.sl_pct = 0.0;
    cfg.exit_defaults.tp_pct = 0.0;
    cfg.risk.max_drawdown       = 0.05;  // 5%
    cfg.risk.max_loss_per_trade = 1e9;   // isolate drawdown path

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    // Must be process-wide terminal, not merely "run loop exited early".
    EXPECT_TRUE(eng.get_halt_flag().load(std::memory_order_acquire));
    // Strategy must have been cut short of the full 200-bar dataset.
    EXPECT_LT(strat->calls(), 200);
}

TEST(LiveSafety, BacktestRiskWorkerDoesNotEscalateResearchRejectionToTerminalHalt)
{
    auto dh = std::make_shared<data_handler>();
    double price = 100.0;
    for (int i = 0; i < 200; ++i)
    {
        const double close = price * (1.0 - 0.01);
        dh->load_into_queue("2024-01-01", "TEST",
                            price, price + 0.1, close - 0.1, close, 1000);
        price = close;
    }

    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<RelentlessBuyer>(10.0);
    MarketMaker mm(3);
    mm.add_orders(ob, 100.0, 100);

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::standard;
    cfg.disable_pinning = true;
    cfg.show_progress = false;
    cfg.initial_balance = 10000.0;
    cfg.risk.max_drawdown = 0.05;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    EXPECT_TRUE(eng.run_succeeded());
    EXPECT_FALSE(eng.is_halted());
}

TEST(LiveSafety, RiskHaltIsWriteOnceTerminal)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;

    engine eng(dh, ob, strat, std::move(cfg));

    // Simulate external/risk-equivalent terminal raise via public API used by
    // watchdog and (after fix) risk paths: exchange(true) write-once.
    eng.trigger_halt("unit-test terminal");
    EXPECT_TRUE(eng.get_halt_flag().load(std::memory_order_acquire));

    // Second raise must be a no-op (still halted).
    eng.trigger_halt("second raise ignored");
    EXPECT_TRUE(eng.get_halt_flag().load(std::memory_order_acquire));
}
