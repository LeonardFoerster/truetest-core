#include <gtest/gtest.h>

#include "engine/live_safety_session.h"
#include "bin/provider_open_policy.h"
#include "providers/provider.h"

#include <atomic>
#include <barrier>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

class CountingReconciler final : public IReconciler
{
public:
    explicit CountingReconciler(bool operational = true)
        : operational_(operational) {}

    bool is_operational() const noexcept override { return operational_; }

    std::string reconcile(const portfolio&, double) override
    {
        ++calls;
        return {};
    }

    std::atomic<int> calls{0};

private:
    bool operational_ = true;
};

class CountingKill final : public IKillSwitch
{
public:
    CountingKill(bool result, std::vector<std::string>* events,
                 std::mutex* events_mu)
        : result_(result), events_(events), events_mu_(events_mu) {}
    bool is_operational() const noexcept override { return true; }
    bool cancel_all_and_flatten(std::chrono::milliseconds) override
    {
        ++calls;
        std::lock_guard<std::mutex> lock(*events_mu_);
        events_->emplace_back("kill");
        return result_;
    }
    std::atomic<int> calls{0};
private:
    bool result_;
    std::vector<std::string>* events_;
    std::mutex* events_mu_;
};

class SessionProvider final : public IProvider
{
public:
    explicit SessionProvider(bool kill_result, bool open_result = true,
                             bool throw_on_open = false,
                             private_execution_capability capability =
                                 private_execution_capability::exchange_writes)
        : reconciler(std::make_shared<CountingReconciler>()),
          kill(std::make_shared<CountingKill>(kill_result, &events, &events_mu)),
          capability_(capability),
          open_result_(open_result), throw_on_open_(throw_on_open) {}

    std::string name() const override { return "session-test"; }
    bool has_data_feed() const override { return true; }
    bool has_execution() const override { return true; }
    private_execution_capability
    private_execution_capability_level() const noexcept override
    {
        return capability_;
    }
    bool prepare_write_safety() override
    {
        ++prepare_calls;
        return prepare_result;
    }
    bool install_write_safety_readiness(
        WriteSafetyReadiness readiness) override
    {
        ++readiness_install_calls;
        if (throw_on_readiness_install)
            throw std::runtime_error("readiness install failure");
        readiness_installed = readiness.permits_private_exchange_writes();
        return readiness_installed;
    }
    bool open() override
    {
        ++open_calls;
        if (capability_ == private_execution_capability::exchange_writes)
            ++private_connect_calls;
        if (block_open)
        {
            std::unique_lock<std::mutex> lock(open_mu);
            open_entered = true;
            open_cv.notify_all();
            open_cv.wait(lock, [this] { return release_open; });
        }
        if (throw_on_open_)
        {
            state = lifecycle::opening;
            throw std::runtime_error("partial open failure");
        }
        state = open_result_ ? lifecycle::open : lifecycle::error;
        return open_result_;
    }
    void close() override { state = lifecycle::closed; }
    lifecycle lifecycle_state() const override { return state; }
    std::shared_ptr<IDataTransport> get_transport() override { return {}; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override { return {}; }
    std::shared_ptr<IReconciler> get_reconciler() override
    {
        if (throw_on_get_reconciler)
            throw std::runtime_error("reconciler lookup failure");
        return reconciler;
    }
    std::shared_ptr<IKillSwitch> get_kill_switch() override { return kill; }
    void quiesce_for_live_shutdown() override
    {
        record("quiesce");
        if (throw_on_quiesce) throw std::runtime_error("quiesce failure");
    }
    void finish_live_shutdown(live_shutdown_disposition d) override
    {
        ++finish_calls;
        disposition = d;
        record("finish");
        if (throw_on_finish) throw std::runtime_error("finish failure");
        state = lifecycle::closed;
    }

    std::shared_ptr<CountingReconciler> reconciler;
    std::shared_ptr<CountingKill> kill;
    std::atomic<int> prepare_calls{0};
    std::atomic<int> readiness_install_calls{0};
    std::atomic<int> open_calls{0};
    std::atomic<int> private_connect_calls{0};
    std::atomic<int> finish_calls{0};
    bool prepare_result = true;
    bool readiness_installed = false;
    bool throw_on_get_reconciler = false;
    bool throw_on_readiness_install = false;
    bool block_open = false;
    bool open_entered = false;
    bool release_open = false;
    bool throw_on_quiesce = false;
    bool throw_on_finish = false;
    lifecycle state = lifecycle::closed;
    live_shutdown_disposition disposition =
        live_shutdown_disposition::preserve_dead_man_switch;
    std::vector<std::string> events;
    std::mutex events_mu;
    std::condition_variable open_cv;
    std::mutex open_mu;

private:
    void record(const char* value)
    {
        std::lock_guard<std::mutex> lock(events_mu);
        events.emplace_back(value);
    }
    private_execution_capability capability_;
    bool open_result_;
    bool throw_on_open_;
};

live_safety_requirements write_requirements(
    std::shared_ptr<IReconciler> reconciler = {},
    std::shared_ptr<IKillSwitch> kill_switch = {})
{
    live_safety_requirements requirements;
    requirements.target_allows_private_exchange_writes = true;
    requirements.private_exchange_execution_requested = true;
    requirements.reconciler = std::move(reconciler);
    requirements.kill_switch = std::move(kill_switch);
    return requirements;
}

} // namespace

TEST(LiveSafetyCapability, NoopComponentsAreUnavailableAndOperationsFail)
{
    NoopReconciler reconciler;
    NoopKillSwitch kill_switch;
    portfolio local{1000.0};

    EXPECT_FALSE(reconciler.is_operational());
    EXPECT_FALSE(reconciler.reconcile(local, 10.0).empty());
    EXPECT_FALSE(kill_switch.is_operational());
    EXPECT_FALSE(kill_switch.cancel_all_and_flatten(
        std::chrono::milliseconds{10}));
}

TEST(LiveSafetyCapability, OperationalTestComponentsAdvertiseCapability)
{
    std::vector<std::string> events;
    std::mutex events_mu;
    CountingReconciler reconciler;
    CountingKill kill_switch(true, &events, &events_mu);

    EXPECT_TRUE(reconciler.is_operational());
    EXPECT_TRUE(kill_switch.is_operational());
}

TEST(LiveSafetyStartup, BacktestAllowsNoopsForProvenReadOnlyExecution)
{
    auto provider = std::make_shared<SessionProvider>(
        true, true, false,
        private_execution_capability::no_private_writes);
    live_safety_requirements requirements;
    requirements.reconciler = std::make_shared<NoopReconciler>();
    requirements.kill_switch = std::make_shared<NoopKillSwitch>();
    LiveSafetySession session(
        provider, std::move(requirements), std::chrono::milliseconds{50});

    EXPECT_TRUE(session.open_provider());
    EXPECT_FALSE(session.permits_private_exchange_writes());
    EXPECT_EQ(provider->prepare_calls.load(), 0);
    EXPECT_EQ(provider->private_connect_calls.load(), 0);
}

TEST(LiveSafetyStartup, ReadOnlyShadowAllowsNoopsWithoutPrivateWrites)
{
    auto provider = std::make_shared<SessionProvider>(
        true, true, false,
        private_execution_capability::no_private_writes);
    live_safety_requirements requirements;
    requirements.reconciler = std::make_shared<NoopReconciler>();
    requirements.kill_switch = std::make_shared<NoopKillSwitch>();
    LiveSafetySession session(
        provider, std::move(requirements), std::chrono::milliseconds{50});

    EXPECT_TRUE(session.open_provider());
    EXPECT_FALSE(session.permits_private_exchange_writes());
    EXPECT_EQ(provider->private_connect_calls.load(), 0);
}

TEST(LiveSafetyStartup, WriteExecutionRejectsNoopReconcilerBeforeConnect)
{
    auto provider = std::make_shared<SessionProvider>(true);
    auto requirements = write_requirements(
        std::make_shared<NoopReconciler>(), provider->kill);
    LiveSafetySession session(
        provider, std::move(requirements), std::chrono::milliseconds{50});
    int worker_starts = 0;

    if (session.open_provider()) ++worker_starts;

    EXPECT_EQ(provider->open_calls.load(), 0);
    EXPECT_EQ(provider->private_connect_calls.load(), 0);
    EXPECT_EQ(worker_starts, 0);
    EXPECT_NE(session.startup_error().find("operational reconciler"),
              std::string::npos);
}

TEST(LiveSafetyStartup, WriteExecutionRejectsNoopKillSwitchBeforeConnect)
{
    auto provider = std::make_shared<SessionProvider>(true);
    auto requirements = write_requirements(
        provider->reconciler, std::make_shared<NoopKillSwitch>());
    LiveSafetySession session(
        provider, std::move(requirements), std::chrono::milliseconds{50});

    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(provider->open_calls.load(), 0);
    EXPECT_EQ(provider->private_connect_calls.load(), 0);
    EXPECT_NE(session.startup_error().find("operational kill switch"),
              std::string::npos);
}

TEST(LiveSafetyStartup, WriteExecutionRejectsBothNoopsWithoutFallback)
{
    auto provider = std::make_shared<SessionProvider>(true);
    auto requirements = write_requirements(
        std::make_shared<NoopReconciler>(),
        std::make_shared<NoopKillSwitch>());
    LiveSafetySession session(
        provider, std::move(requirements), std::chrono::milliseconds{50});

    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(provider->open_calls.load(), 0);
    EXPECT_EQ(provider->kill->calls.load(), 0)
        << "an invalid explicit override must not fall back to provider safety";
}

TEST(LiveSafetyStartup, OperationalWriteSafetyValidatesBeforePrivateConnect)
{
    auto provider = std::make_shared<SessionProvider>(true);
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});

    ASSERT_TRUE(session.open_provider());
    EXPECT_TRUE(session.startup_safety_validated());
    EXPECT_TRUE(session.permits_private_exchange_writes());
    EXPECT_EQ(provider->prepare_calls.load(), 1);
    EXPECT_EQ(provider->readiness_install_calls.load(), 1);
    EXPECT_TRUE(provider->readiness_installed);
    EXPECT_EQ(provider->private_connect_calls.load(), 1);
}

TEST(LiveSafetyStartup, SessionIsNotOpenWhileProviderOpenIsInProgress)
{
    auto provider = std::make_shared<SessionProvider>(true);
    provider->block_open = true;
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});
    bool open_result = false;
    std::thread opener([&] { open_result = session.open_provider(); });

    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(provider->open_mu);
        entered = provider->open_cv.wait_for(
            lock, std::chrono::seconds{1},
            [&] { return provider->open_entered; });
    }
    EXPECT_TRUE(entered);
    EXPECT_FALSE(session.is_open());
    {
        std::lock_guard<std::mutex> lock(provider->open_mu);
        provider->release_open = true;
    }
    provider->open_cv.notify_all();
    opener.join();

    EXPECT_TRUE(open_result);
    EXPECT_TRUE(session.is_open());
}

TEST(LiveSafetyStartup, NonLiveTargetRejectsPrivateWriteProvider)
{
    auto provider = std::make_shared<SessionProvider>(true);
    live_safety_requirements requirements;
    requirements.private_exchange_execution_requested = true;
    requirements.reconciler = provider->reconciler;
    requirements.kill_switch = provider->kill;
    LiveSafetySession session(
        provider, std::move(requirements), std::chrono::milliseconds{50});

    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(provider->prepare_calls.load(), 0);
    EXPECT_EQ(provider->open_calls.load(), 0);
    EXPECT_NE(session.startup_error().find("target does not allow"),
              std::string::npos);
}

TEST(LiveSafetyStartup, WriteRequestRejectsTechnicallyReadOnlyProvider)
{
    auto provider = std::make_shared<SessionProvider>(
        true, true, false,
        private_execution_capability::no_private_writes);
    auto requirements = write_requirements(
        provider->reconciler, provider->kill);
    LiveSafetySession session(
        provider, std::move(requirements), std::chrono::milliseconds{50});

    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(provider->prepare_calls.load(), 0);
    EXPECT_EQ(provider->open_calls.load(), 0);
    EXPECT_NE(session.startup_error().find("cannot issue private exchange writes"),
              std::string::npos);
}

TEST(LiveSafetyStartup, ReadOnlyRequestRejectsPrivateWriteProvider)
{
    auto provider = std::make_shared<SessionProvider>(true);
    live_safety_requirements requirements;
    requirements.target_allows_private_exchange_writes = true;
    LiveSafetySession session(
        provider, std::move(requirements), std::chrono::milliseconds{50});

    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(provider->prepare_calls.load(), 0);
    EXPECT_EQ(provider->open_calls.load(), 0);
    EXPECT_NE(session.startup_error().find("read-only execution selected"),
              std::string::npos);
}

TEST(LiveSafetyStartup, UndeclaredExecutionCapabilityFailsClosed)
{
    auto provider = std::make_shared<SessionProvider>(
        true, true, false, private_execution_capability::unknown);
    live_safety_requirements requirements;
    LiveSafetySession session(
        provider, std::move(requirements), std::chrono::milliseconds{50});

    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(provider->open_calls.load(), 0);
    EXPECT_NE(session.startup_error().find("capability is undeclared"),
              std::string::npos);
}

TEST(LiveSafetyStartup, FailedRealSafetyPreparationHasNoNoopFallback)
{
    auto provider = std::make_shared<SessionProvider>(true);
    provider->prepare_result = false;
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});

    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(provider->prepare_calls.load(), 1);
    EXPECT_EQ(provider->open_calls.load(), 0);
    EXPECT_NE(session.startup_error().find("prepare operational safety"),
              std::string::npos);
}

TEST(LiveSafetyStartup, ThrowingSafetyLookupRejectsWithoutConnectOrHang)
{
    auto provider = std::make_shared<SessionProvider>(true);
    provider->throw_on_get_reconciler = true;
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});

    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(provider->open_calls.load(), 0);
    EXPECT_EQ(provider->private_connect_calls.load(), 0);
    EXPECT_NE(session.startup_error().find("preflight threw"),
              std::string::npos);
}

TEST(LiveSafetyStartup, ThrowingReadinessInstallRejectsBeforeConnect)
{
    auto provider = std::make_shared<SessionProvider>(true);
    provider->throw_on_readiness_install = true;
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});

    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(provider->readiness_install_calls.load(), 1);
    EXPECT_EQ(provider->open_calls.load(), 0);
    EXPECT_EQ(provider->private_connect_calls.load(), 0);
    EXPECT_NE(session.startup_error().find("preflight threw"),
              std::string::npos);
}

TEST(LiveSafetySession, RepeatedAndConcurrentShutdownKillsExactlyOnce)
{
    auto provider = std::make_shared<SessionProvider>(true);
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});
    ASSERT_TRUE(session.open_provider());

    std::barrier gate(3);
    live_shutdown_report a, b;
    std::thread one([&] { gate.arrive_and_wait(); a = session.shutdown_once(live_shutdown_reason::engine_halt); });
    std::thread two([&] { gate.arrive_and_wait(); b = session.shutdown_once(live_shutdown_reason::normal_end); });
    gate.arrive_and_wait();
    one.join();
    two.join();

    EXPECT_EQ(provider->kill->calls.load(), 1);
    EXPECT_TRUE(a.kill_succeeded);
    EXPECT_TRUE(b.kill_succeeded);
    EXPECT_EQ(provider->disposition, live_shutdown_disposition::disarm_after_kill);
    ASSERT_EQ(provider->events.size(), 3u);
    EXPECT_EQ(provider->events[0], "quiesce");
    EXPECT_EQ(provider->events[1], "kill");
    EXPECT_EQ(provider->events[2], "finish");
}

TEST(LiveSafetySession, FailedKillPreservesDeadManSwitch)
{
    auto provider = std::make_shared<SessionProvider>(false);
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});
    ASSERT_TRUE(session.open_provider());
    auto report = session.shutdown_once(live_shutdown_reason::engine_halt);

    EXPECT_TRUE(report.kill_attempted);
    EXPECT_FALSE(report.kill_succeeded);
    EXPECT_EQ(provider->kill->calls.load(), 1);
    EXPECT_EQ(provider->disposition,
              live_shutdown_disposition::preserve_dead_man_switch);
}

TEST(LiveSafetySession, PartialLiveOpenWithKillSwitchUsesKillBeforeClose)
{
    auto provider = std::make_shared<SessionProvider>(true, false);
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});
    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(provider->kill->calls.load(), 1);
    ASSERT_EQ(provider->events.size(), 3u);
    EXPECT_EQ(provider->events[0], "quiesce");
    EXPECT_EQ(provider->events[1], "kill");
    EXPECT_EQ(provider->events[2], "finish");
}

TEST(LiveSafetySession, ThrowingPartialOpenStillKillsBeforeClose)
{
    auto provider = std::make_shared<SessionProvider>(true, true, true);
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});
    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(provider->open_calls.load(), 1);
    EXPECT_EQ(provider->kill->calls.load(), 1);
    ASSERT_EQ(provider->events.size(), 3u);
    EXPECT_EQ(provider->events[0], "quiesce");
    EXPECT_EQ(provider->events[1], "kill");
    EXPECT_EQ(provider->events[2], "finish");
}

TEST(LiveSafetySession, ShutdownBeforeOpenPermanentlyRefusesOpen)
{
    auto provider = std::make_shared<SessionProvider>(true);
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});
    (void)session.shutdown_once(live_shutdown_reason::normal_end);
    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(provider->open_calls.load(), 0);
}

TEST(LiveSafetySession, ExplicitKillSwitchRemainsSessionOwnedAndExactOnce)
{
    auto provider = std::make_shared<SessionProvider>(false);
    auto override_kill = std::make_shared<CountingKill>(
        true, &provider->events, &provider->events_mu);
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});
    ASSERT_TRUE(session.set_kill_switch(override_kill));
    ASSERT_TRUE(session.open_provider());
    auto first = session.shutdown_once(live_shutdown_reason::operator_kill);
    auto second = session.shutdown_once(live_shutdown_reason::normal_end);
    EXPECT_TRUE(first.kill_succeeded);
    EXPECT_TRUE(second.kill_succeeded);
    EXPECT_EQ(override_kill->calls.load(), 1);
    EXPECT_EQ(provider->kill->calls.load(), 0);
    EXPECT_FALSE(session.set_kill_switch(provider->kill));
}

TEST(LiveSafetySession, QuiesceFailureStillKillsAndFinishes)
{
    auto provider = std::make_shared<SessionProvider>(true);
    provider->throw_on_quiesce = true;
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});
    ASSERT_TRUE(session.open_provider());

    const auto report = session.shutdown_once(live_shutdown_reason::engine_halt);

    EXPECT_FALSE(report.quiesce_succeeded);
    EXPECT_TRUE(report.kill_succeeded);
    EXPECT_TRUE(report.provider_closed);
    EXPECT_EQ(provider->disposition,
              live_shutdown_disposition::preserve_dead_man_switch);
    EXPECT_EQ(provider->kill->calls.load(), 1);
    EXPECT_EQ(provider->finish_calls.load(), 1);
    ASSERT_EQ(provider->events.size(), 3u);
    EXPECT_EQ(provider->events[0], "quiesce");
    EXPECT_EQ(provider->events[1], "kill");
    EXPECT_EQ(provider->events[2], "finish");
}

TEST(LiveSafetySession, FinishFailureIsCachedWithoutRetry)
{
    auto provider = std::make_shared<SessionProvider>(true);
    provider->throw_on_finish = true;
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});
    ASSERT_TRUE(session.open_provider());

    const auto first = session.shutdown_once(live_shutdown_reason::engine_halt);
    const auto second = session.shutdown_once(live_shutdown_reason::normal_end);

    EXPECT_TRUE(first.kill_succeeded);
    EXPECT_FALSE(first.provider_closed);
    EXPECT_EQ(second.kill_succeeded, first.kill_succeeded);
    EXPECT_EQ(second.provider_closed, first.provider_closed);
    EXPECT_EQ(provider->kill->calls.load(), 1);
    EXPECT_EQ(provider->finish_calls.load(), 1);
}

TEST(LiveSafetySession, GuardedProviderFailureShutsDownBeforeReturning)
{
    auto provider = std::make_shared<SessionProvider>(true);
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});
    ASSERT_TRUE(session.open_provider());
    std::ostringstream errors;

    const int rc = truetest::bin::run_provider_session_guarded(
        session,
        []() -> int { throw std::runtime_error("constructor refused"); },
        errors);

    EXPECT_EQ(rc, 1);
    EXPECT_EQ(provider->kill->calls.load(), 1);
    EXPECT_EQ(provider->disposition,
              live_shutdown_disposition::disarm_after_kill);
    ASSERT_EQ(provider->events.size(), 3u);
    EXPECT_EQ(provider->events[0], "quiesce");
    EXPECT_EQ(provider->events[1], "kill");
    EXPECT_EQ(provider->events[2], "finish");
    EXPECT_NE(errors.str().find("constructor refused"), std::string::npos);

    (void)session.shutdown_once(live_shutdown_reason::normal_end);
    EXPECT_EQ(provider->kill->calls.load(), 1);
}

TEST(LiveSafetySession, GuardedProviderFailurePreservesDmsWhenKillFails)
{
    auto provider = std::make_shared<SessionProvider>(false);
    LiveSafetySession session(
        provider, write_requirements(), std::chrono::milliseconds{50});
    ASSERT_TRUE(session.open_provider());
    std::ostringstream errors;

    const int rc = truetest::bin::run_provider_session_guarded(
        session,
        []() -> int { throw 7; },
        errors);

    EXPECT_EQ(rc, 1);
    EXPECT_EQ(provider->kill->calls.load(), 1);
    EXPECT_EQ(provider->disposition,
              live_shutdown_disposition::preserve_dead_man_switch);
    EXPECT_NE(errors.str().find("unknown exception"), std::string::npos);
}
