#include <gtest/gtest.h>

#include "threading/worker_watchdog.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

namespace {

struct SilenceStderr
{
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceStderr() : orig(std::cerr.rdbuf(sink.rdbuf())) {}
    ~SilenceStderr() { std::cerr.rdbuf(orig); }
};

}

// --- Pure decision logic ----------------------------------------------------

TEST(WorkerWatchdog, IsHungNeverBeatenIsAlive)
{
    // last_alive_ms <= 0 means the source hasn't reported yet; we
    // wait for its first beat before we start the deadline clock.
    EXPECT_FALSE(WorkerWatchdog::is_hung(/*now=*/1000, /*last=*/0, /*deadline=*/100));
    EXPECT_FALSE(WorkerWatchdog::is_hung(/*now=*/1000, /*last=*/-5, /*deadline=*/100));
}

TEST(WorkerWatchdog, IsHungZeroDeadlineMeansDisabled)
{
    EXPECT_FALSE(WorkerWatchdog::is_hung(/*now=*/1000, /*last=*/100, /*deadline=*/0));
    EXPECT_FALSE(WorkerWatchdog::is_hung(/*now=*/1000, /*last=*/100, /*deadline=*/-1));
}

TEST(WorkerWatchdog, IsHungWithinDeadlineIsAlive)
{
    // 50ms gap, 100ms deadline → still alive.
    EXPECT_FALSE(WorkerWatchdog::is_hung(/*now=*/1000, /*last=*/950, /*deadline=*/100));
}

TEST(WorkerWatchdog, IsHungAtDeadlineBoundaryIsAlive)
{
    // Strictly greater-than: a beat exactly at the deadline boundary
    // is still alive. Choosing > avoids spurious halts at the moment
    // the source posts its beat.
    EXPECT_FALSE(WorkerWatchdog::is_hung(/*now=*/1100, /*last=*/1000, /*deadline=*/100));
}

TEST(WorkerWatchdog, IsHungPastDeadlineFires)
{
    EXPECT_TRUE(WorkerWatchdog::is_hung(/*now=*/1101, /*last=*/1000, /*deadline=*/100));
    EXPECT_TRUE(WorkerWatchdog::is_hung(/*now=*/2000, /*last=*/1000, /*deadline=*/100));
}

// --- Threaded behaviour -----------------------------------------------------
// All threaded tests use small intervals (poll 20ms, deadline 80ms) so
// they finish in well under a second. Tighten further only carefully —
// scheduling jitter on heavily-loaded CI can produce flakes below ~10ms.

TEST(WorkerWatchdog, NotStartedDoesNothing)
{
    WorkerWatchdog wd(std::chrono::milliseconds(20));
    std::atomic<int64_t> last{0};
    wd.register_source("idle", &last, 80);
    std::atomic<bool> halt{false};
    wd.set_halt_flag(halt);
    // Never start the watchdog. Nothing should happen.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_FALSE(halt.load());
    EXPECT_FALSE(wd.triggered());
}

TEST(WorkerWatchdog, AliveSourceDoesNotFire)
{
    SilenceStderr quiet;

    WorkerWatchdog wd(std::chrono::milliseconds(20));
    std::atomic<int64_t> last{0};
    wd.register_source("alive", &last, 80);
    std::atomic<bool> halt{false};
    wd.set_halt_flag(halt);
    wd.start();

    // Beat every 20ms for 200ms. Deadline is 80ms; gap is 20ms. Never hung.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline)
    {
        last.store(WorkerWatchdog::now_monotonic_ms(), std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    wd.stop();
    EXPECT_FALSE(halt.load()) << "halt should NOT fire while source beats within deadline";
    EXPECT_FALSE(wd.triggered());
}

TEST(WorkerWatchdog, SilentSourceFiresHaltOnceDeadlinePasses)
{
    SilenceStderr quiet;

    WorkerWatchdog wd(std::chrono::milliseconds(20));
    std::atomic<int64_t> last{0};
    wd.register_source("dead", &last, 80);
    std::atomic<bool> halt{false};
    wd.set_halt_flag(halt);

    // Beat once to start the deadline clock, then go silent.
    last.store(WorkerWatchdog::now_monotonic_ms(), std::memory_order_release);
    wd.start();

    // Wait long enough for: deadline (80ms) + 2 poll intervals (40ms) + slack.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    EXPECT_TRUE(halt.load()) << "halt must fire when deadline passes with no beat";
    EXPECT_TRUE(wd.triggered());

    wd.stop();
}

TEST(WorkerWatchdog, MultipleSourcesOnlyLaggardTriggers)
{
    SilenceStderr quiet;

    WorkerWatchdog wd(std::chrono::milliseconds(20));
    std::atomic<int64_t> healthy{0};
    std::atomic<int64_t> stuck{0};
    wd.register_source("healthy", &healthy, 80);
    wd.register_source("stuck",   &stuck,   80);
    std::atomic<bool> halt{false};
    wd.set_halt_flag(halt);

    // Both sources beat once at start, but only `healthy` keeps beating.
    healthy.store(WorkerWatchdog::now_monotonic_ms(), std::memory_order_release);
    stuck.store(WorkerWatchdog::now_monotonic_ms(),   std::memory_order_release);

    wd.start();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < deadline)
    {
        healthy.store(WorkerWatchdog::now_monotonic_ms(), std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // The stuck source's missed deadline should have fired the halt.
    EXPECT_TRUE(halt.load());
    EXPECT_TRUE(wd.triggered());

    wd.stop();
}

TEST(WorkerWatchdog, SingleShotDoesNotRespamAfterHalt)
{
    SilenceStderr quiet;

    WorkerWatchdog wd(std::chrono::milliseconds(20));
    std::atomic<int64_t> last{0};
    wd.register_source("dead", &last, 80);
    std::atomic<bool> halt{false};
    wd.set_halt_flag(halt);

    last.store(WorkerWatchdog::now_monotonic_ms(), std::memory_order_release);
    wd.start();

    // Wait for first detection.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ASSERT_TRUE(wd.triggered());

    // Capture the stderr length now that the warning has been printed,
    // then sleep through several more poll intervals. Stderr should
    // gain nothing further: the watchdog stops itself after the first
    // hang detection.
    const auto sink_size_after_first = quiet.sink.tellp();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto sink_size_later = quiet.sink.tellp();
    EXPECT_EQ(sink_size_after_first, sink_size_later)
        << "watchdog must not re-log on subsequent polls after halt";

    wd.stop();
}

TEST(WorkerWatchdog, StopJoinsClenly)
{
    WorkerWatchdog wd(std::chrono::milliseconds(20));
    std::atomic<int64_t> last{0};
    wd.register_source("idle", &last, 80);

    wd.start();
    // Don't trigger — just let the watchdog run idle for a bit.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    wd.stop();
    // If stop() returns at all, the thread joined. Anything else
    // would have hung the test.
    SUCCEED();
}

TEST(WorkerWatchdog, RestartAfterStopIsClean)
{
    WorkerWatchdog wd(std::chrono::milliseconds(20));
    std::atomic<int64_t> last{0};
    wd.register_source("idle", &last, 80);

    wd.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    wd.stop();
    EXPECT_FALSE(wd.triggered());

    // Second start should not throw and the triggered_ flag should
    // be reset so a new run starts from a clean slate.
    wd.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    wd.stop();
    EXPECT_FALSE(wd.triggered());
}
