#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_futures_dead_mans_switch.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

namespace {

struct SilenceStderr
{
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceStderr() : orig(std::cerr.rdbuf(sink.rdbuf())) {}
    ~SilenceStderr() { std::cerr.rdbuf(orig); }
};

using response = BinanceFuturesDeadMansSwitch::response;

// Records each call and lets the test program canned responses per call.
// If `responses` is empty, returns `default_resp` for every call.
struct fake_post
{
    response default_resp{200, "{}"};
    std::vector<response> responses;
    std::vector<std::pair<std::string, std::string>> log;
    std::mutex mu;

    response operator()(std::string_view ep, std::string_view p)
    {
        std::lock_guard<std::mutex> lk(mu);
        log.emplace_back(std::string(ep), std::string(p));
        if (log.size() <= responses.size())
            return responses[log.size() - 1];
        return default_resp;
    }

    std::size_t call_count()
    {
        std::lock_guard<std::mutex> lk(mu);
        return log.size();
    }
};

std::function<response(std::string_view, std::string_view)>
wrap(std::shared_ptr<fake_post> f)
{
    return [f](std::string_view ep, std::string_view p) {
        return (*f)(ep, p);
    };
}

}

TEST(BinanceFuturesDeadMansSwitch, StartArmsCountdown)
{
    auto post = std::make_shared<fake_post>();
    BinanceFuturesDeadMansSwitch dms(wrap(post), "BTCUSDT",
                                      /*countdown_ms=*/30000,
                                      /*heartbeat_ms=*/100);

    ASSERT_TRUE(dms.start());

    // Startup posts an initial arm with the configured countdown.
    ASSERT_GE(post->call_count(), 1u);
    EXPECT_EQ(post->log[0].first, "/fapi/v1/countdownCancelAll");
    EXPECT_NE(post->log[0].second.find("symbol=BTCUSDT"),    std::string::npos);
    EXPECT_NE(post->log[0].second.find("countdownTime=30000"), std::string::npos);

    dms.stop();
}

TEST(BinanceFuturesDeadMansSwitch, StartFailsIfInitialArmFails)
{
    SilenceStderr quiet;

    auto post = std::make_shared<fake_post>();
    post->responses = {{500, "{\"msg\":\"server error\"}"}};

    BinanceFuturesDeadMansSwitch dms(wrap(post), "BTCUSDT", 30000, 100);
    EXPECT_FALSE(dms.start())
        << "initial arm failure must propagate; can't go live without protection";
    // No heartbeat thread should be spawned.
    EXPECT_EQ(post->call_count(), 1u);
}

TEST(BinanceFuturesDeadMansSwitch, HeartbeatRefreshes)
{
    auto post = std::make_shared<fake_post>();
    BinanceFuturesDeadMansSwitch dms(wrap(post), "BTCUSDT",
                                      /*countdown_ms=*/30000,
                                      /*heartbeat_ms=*/30);

    ASSERT_TRUE(dms.start());

    // Wait for several heartbeat cycles. With heartbeat_ms=30 and
    // 200ms wall, we expect ≥ 4 calls beyond the initial arm.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    dms.stop();

    EXPECT_GE(post->call_count(), 5u)
        << "heartbeat should refresh the countdown periodically";
    // All calls hit the same endpoint with the same countdown value.
    for (const auto& [ep, params] : post->log)
    {
        EXPECT_EQ(ep, "/fapi/v1/countdownCancelAll");
        EXPECT_NE(params.find("countdownTime=30000"), std::string::npos);
    }
}

TEST(BinanceFuturesDeadMansSwitch, LivenessTsAdvancesOnHeartbeat)
{
    auto post = std::make_shared<fake_post>();
    BinanceFuturesDeadMansSwitch dms(wrap(post), "BTCUSDT",
                                      /*countdown_ms=*/30000,
                                      /*heartbeat_ms=*/30);

    ASSERT_TRUE(dms.start());

    const int64_t initial = dms.liveness_ts().load(std::memory_order_acquire);
    EXPECT_GT(initial, 0)
        << "initial arm should bump the liveness atomic so the watchdog "
           "knows the source has reported at least once";

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const int64_t later = dms.liveness_ts().load(std::memory_order_acquire);
    EXPECT_GT(later, initial)
        << "heartbeat must advance liveness so the watchdog stays happy";

    dms.stop();
}

TEST(BinanceFuturesDeadMansSwitch, HeartbeatRetriesOnceOnFailure)
{
    SilenceStderr quiet;

    // Initial arm OK, then alternating fail/recover. The retry budget
    // is 1 attempt after a 500ms pause, so a single transient failure
    // should still update the liveness ts on the retry.
    auto post = std::make_shared<fake_post>();
    post->responses = {
        {200, "{}"},                                  // initial arm
        {500, "{\"msg\":\"transient\"}"},             // first heartbeat fails
        {200, "{}"},                                  // retry succeeds
        {200, "{}"},                                  // subsequent heartbeats
        {200, "{}"},
        {200, "{}"},
        {200, "{}"},
    };

    BinanceFuturesDeadMansSwitch dms(wrap(post), "BTCUSDT",
                                      /*countdown_ms=*/30000,
                                      /*heartbeat_ms=*/50);
    ASSERT_TRUE(dms.start());

    // Need to wait for: initial heartbeat tick (~50ms) + retry pause
    // (500ms) + retry attempt + a couple more cycles. ~800ms is safe.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    dms.stop();

    // ≥ 4 calls: initial arm + first heartbeat (failed) + retry + later
    // beats. Exact count is timing-dependent so we use a lower bound.
    EXPECT_GE(post->call_count(), 4u);
}

TEST(BinanceFuturesDeadMansSwitch, DisarmPostsCountdownZero)
{
    auto post = std::make_shared<fake_post>();
    BinanceFuturesDeadMansSwitch dms(wrap(post), "BTCUSDT", 30000, 100);

    ASSERT_TRUE(dms.start());
    dms.stop();
    const auto count_before_disarm = post->call_count();

    EXPECT_TRUE(dms.disarm());
    ASSERT_EQ(post->call_count(), count_before_disarm + 1);
    EXPECT_NE(post->log.back().second.find("countdownTime=0"), std::string::npos);
}

TEST(BinanceFuturesDeadMansSwitch, StopJoinsCleanly)
{
    auto post = std::make_shared<fake_post>();
    BinanceFuturesDeadMansSwitch dms(wrap(post), "BTCUSDT", 30000, 100);

    ASSERT_TRUE(dms.start());
    dms.stop();
    // If stop() returns at all, the heartbeat thread joined.
    SUCCEED();
}

TEST(BinanceFuturesDeadMansSwitch, DoubleStopIsSafe)
{
    auto post = std::make_shared<fake_post>();
    BinanceFuturesDeadMansSwitch dms(wrap(post), "BTCUSDT", 30000, 100);

    ASSERT_TRUE(dms.start());
    dms.stop();
    dms.stop();   // must not crash or hang
    SUCCEED();
}

TEST(BinanceFuturesDeadMansSwitch, StopBeforeStartIsNoop)
{
    auto post = std::make_shared<fake_post>();
    BinanceFuturesDeadMansSwitch dms(wrap(post), "BTCUSDT", 30000, 100);
    dms.stop();
    EXPECT_EQ(post->call_count(), 0u);
}

// Phase 3: when attempt_close=true and persistent post failures occur,
// the close_position_fn must be invoked exactly once from the heartbeat
// thread (before liveness goes permanently stale).
TEST(BinanceFuturesDeadMansSwitch, PersistentFailureInvokesCloseFn)
{
    SilenceStderr quiet;

    struct CloseRecorder
    {
        std::string last_symbol;
        int call_count = 0;
        void operator()(const std::string& sym)
        {
            last_symbol = sym;
            ++call_count;
        }
    };

    auto post = std::make_shared<fake_post>();
    // Initial arm succeeds, then every subsequent heartbeat fails.
    post->responses = {
        {200, "{}"},                                  // initial arm
        {500, "{\"msg\":\"down\"}"},
        {500, "{\"msg\":\"down\"}"},
        {500, "{\"msg\":\"down\"}"},
    };
    post->default_resp = {500, "{\"msg\":\"down\"}"};

    CloseRecorder recorder;
    BinanceFuturesDeadMansSwitch dms(
        wrap(post), "BTCUSDT",
        /*countdown_ms=*/30000,
        /*heartbeat_ms=*/20,   // fast for test
        /*attempt_close=*/true,
        /*closer=*/[&recorder](const std::string& s){ recorder(s); });

    ASSERT_TRUE(dms.start());

    // Give time for arm + at least two heartbeats + the failure branch.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    dms.stop();

    EXPECT_GE(post->call_count(), 3u) << "arm + at least two failed heartbeats";
    EXPECT_EQ(recorder.call_count, 1) << "close fn must fire exactly once on persistent failure";
    EXPECT_EQ(recorder.last_symbol, "BTCUSDT");
}

#endif // HAS_BINANCE
