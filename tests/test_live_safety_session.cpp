#include <gtest/gtest.h>

#include "engine/live_safety_session.h"
#include "bin/provider_open_policy.h"
#include "providers/provider.h"

#include <atomic>
#include <barrier>
#include <mutex>
#include <sstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

class CountingKill final : public IKillSwitch
{
public:
    CountingKill(bool result, std::vector<std::string>* events,
                 std::mutex* events_mu)
        : result_(result), events_(events), events_mu_(events_mu) {}
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
                             bool throw_on_open = false)
        : kill(std::make_shared<CountingKill>(kill_result, &events, &events_mu)),
          open_result_(open_result), throw_on_open_(throw_on_open) {}

    std::string name() const override { return "session-test"; }
    bool has_data_feed() const override { return true; }
    bool has_execution() const override { return true; }
    bool open() override
    {
        ++open_calls;
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

    std::shared_ptr<CountingKill> kill;
    std::atomic<int> open_calls{0};
    std::atomic<int> finish_calls{0};
    bool throw_on_quiesce = false;
    bool throw_on_finish = false;
    lifecycle state = lifecycle::closed;
    live_shutdown_disposition disposition =
        live_shutdown_disposition::preserve_dead_man_switch;
    std::vector<std::string> events;
    std::mutex events_mu;

private:
    void record(const char* value)
    {
        std::lock_guard<std::mutex> lock(events_mu);
        events.emplace_back(value);
    }
    bool open_result_;
    bool throw_on_open_;
};

} // namespace

TEST(LiveSafetySession, RepeatedAndConcurrentShutdownKillsExactlyOnce)
{
    auto provider = std::make_shared<SessionProvider>(true);
    LiveSafetySession session(provider, true, std::chrono::milliseconds{50});
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
    LiveSafetySession session(provider, true, std::chrono::milliseconds{50});
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
    LiveSafetySession session(provider, true, std::chrono::milliseconds{50});
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
    LiveSafetySession session(provider, true, std::chrono::milliseconds{50});
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
    LiveSafetySession session(provider, true, std::chrono::milliseconds{50});
    (void)session.shutdown_once(live_shutdown_reason::normal_end);
    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(provider->open_calls.load(), 0);
}

TEST(LiveSafetySession, ExplicitKillSwitchRemainsSessionOwnedAndExactOnce)
{
    auto provider = std::make_shared<SessionProvider>(false);
    auto override_kill = std::make_shared<CountingKill>(
        true, &provider->events, &provider->events_mu);
    LiveSafetySession session(provider, true, std::chrono::milliseconds{50});
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
    LiveSafetySession session(provider, true, std::chrono::milliseconds{50});
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
    LiveSafetySession session(provider, true, std::chrono::milliseconds{50});
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
    LiveSafetySession session(provider, true, std::chrono::milliseconds{50});
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
    LiveSafetySession session(provider, true, std::chrono::milliseconds{50});
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
