#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_futures_dead_mans_switch.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct SilenceStderr
{
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceStderr() : orig(std::cerr.rdbuf(sink.rdbuf())) {}
    ~SilenceStderr() { std::cerr.rdbuf(orig); }
};

using response = BitgetFuturesDeadMansSwitch::response;

struct fake_post
{
    response default_resp{200, R"({"code":"00000","msg":"success","data":"success"})"};
    std::vector<response> responses;
    std::vector<std::pair<std::string, std::string>> log;
    std::mutex mu;
    std::condition_variable cv;

    response operator()(std::string_view ep, std::string_view body)
    {
        std::lock_guard<std::mutex> lk(mu);
        log.emplace_back(std::string(ep), std::string(body));
        const auto response = log.size() <= responses.size()
            ? responses[log.size() - 1] : default_resp;
        cv.notify_all();
        return response;
    }

    bool wait_for_calls(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, timeout, [&] { return log.size() >= count; });
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
    return [f](std::string_view ep, std::string_view body) {
        return (*f)(ep, body);
    };
}

} // namespace

TEST(BitgetFuturesDeadMansSwitch, ClampCountdownSec)
{
    SilenceStderr quiet;
    EXPECT_EQ(BitgetFuturesDeadMansSwitch::clamp_countdown_sec(40000), 40);
    EXPECT_EQ(BitgetFuturesDeadMansSwitch::clamp_countdown_sec(5000), 5);
    EXPECT_EQ(BitgetFuturesDeadMansSwitch::clamp_countdown_sec(60000), 60);
    EXPECT_EQ(BitgetFuturesDeadMansSwitch::clamp_countdown_sec(120000), 60);
    // Sub-5s configs WARN+clamp to 5.
    EXPECT_EQ(BitgetFuturesDeadMansSwitch::clamp_countdown_sec(3000), 5);
    EXPECT_EQ(BitgetFuturesDeadMansSwitch::clamp_countdown_sec(1), 5);
}

TEST(BitgetFuturesDeadMansSwitch, UpperCountdownClampWarns)
{
    SilenceStderr quiet;
    EXPECT_EQ(BitgetFuturesDeadMansSwitch::clamp_countdown_sec(200000), 60);
    EXPECT_NE(quiet.sink.str().find("clamping to 60s"), std::string::npos);
}

// Binance-style large ms: countdown clamps to 60s; raw hb = 200000/3 would
// exceed the venue timer — ctor must reduce HB below countdown.
TEST(BitgetFuturesDeadMansSwitch, LargeCountdownClampsHbBelowTimer)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    // Simulate provider that still passed raw/3 (pre-fix) or operator
    // set an oversized heartbeat alongside a large countdown.
    const int64_t raw_countdown = 200000;
    const int64_t raw_hb = raw_countdown / 3; // 66666 ≥ 60000 after clamp
    BitgetFuturesDeadMansSwitch dms(wrap(post), raw_countdown, raw_hb);

    EXPECT_EQ(dms.countdown_sec(), 60);
    EXPECT_EQ(dms.countdown_ms(), 60000);
    EXPECT_LT(dms.heartbeat_interval_ms(), 60000);
    EXPECT_EQ(dms.heartbeat_interval_ms(), 20000); // max(1000, 60000/3)
    EXPECT_NE(quiet.sink.str().find("clamping to 60s"), std::string::npos);
    EXPECT_NE(quiet.sink.str().find("reducing to"), std::string::npos);
}

TEST(BitgetFuturesDeadMansSwitch, ZeroHeartbeatUsesCountdownThird)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    BitgetFuturesDeadMansSwitch dms(wrap(post),
                                    /*countdown_ms=*/30000,
                                    /*heartbeat_ms=*/0);
    EXPECT_EQ(dms.countdown_sec(), 30);
    EXPECT_EQ(dms.heartbeat_interval_ms(), 10000); // 30000/3
}

TEST(BitgetFuturesDeadMansSwitch, StartArmsCountdown)
{
    SilenceStderr quiet; // account-wide caveat + arm logs
    auto post = std::make_shared<fake_post>();
    BitgetFuturesDeadMansSwitch dms(wrap(post),
                                    /*countdown_ms=*/30000,
                                    /*heartbeat_ms=*/1000);

    ASSERT_TRUE(dms.start());
    EXPECT_EQ(dms.countdown_sec(), 30);

    ASSERT_GE(post->call_count(), 1u);
    EXPECT_EQ(post->log[0].first, "/api/v3/trade/countdown-cancel-all");
    EXPECT_NE(post->log[0].second.find("\"countdown\":\"30\""),
              std::string::npos);

    dms.stop();
}

TEST(BitgetFuturesDeadMansSwitch, StartFailsIfInitialArmFails)
{
    SilenceStderr quiet;

    auto post = std::make_shared<fake_post>();
    post->responses = {{500, R"({"code":"50000","msg":"server error"})"}};

    BitgetFuturesDeadMansSwitch dms(wrap(post), 30000, 1000);
    EXPECT_FALSE(dms.start())
        << "initial arm failure must refuse start / live open";
    EXPECT_EQ(post->call_count(), 1u);
}

TEST(BitgetFuturesDeadMansSwitch, ArmRequiresAuthoritativeSuccessEnvelope)
{
    SilenceStderr quiet;
    const std::vector<std::string> malformed = {
        R"({"code":"00000"})",
        R"({"code":"00000","msg":"success","data":{}})",
        R"({"code":"00000","msg":"success","nested":{"data":"success"}})",
        R"({"code":"00000","code":"40000","msg":"success","data":"success"})",
        R"({"code":"00000","msg":"success","data":"wrong"})",
        R"({"code":"00000","msg":"success","data":"success"} trailing)",
    };

    for (const auto& body : malformed)
    {
        auto post = std::make_shared<fake_post>();
        post->responses = {{200, body}};
        BitgetFuturesDeadMansSwitch dms(wrap(post), 30000, 1000);
        EXPECT_FALSE(dms.start()) << body;
        EXPECT_EQ(post->call_count(), 1u);
    }
}

TEST(BitgetFuturesDeadMansSwitch, PermissionFailureLogsBdHint)
{
    SilenceStderr quiet;

    auto post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"40014","msg":"permission denied for countdown"})"},
    };

    BitgetFuturesDeadMansSwitch dms(wrap(post), 30000, 1000);
    EXPECT_FALSE(dms.start());
    // Captured stderr should mention BD enablement.
    EXPECT_NE(quiet.sink.str().find("BD enablement"), std::string::npos);
}

TEST(BitgetFuturesDeadMansSwitch, HeartbeatRefreshes)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    // heartbeat_ms floor is 1000; use 1000 and wait ~2.5s for ≥2 beats
    // beyond the initial arm.
    BitgetFuturesDeadMansSwitch dms(wrap(post),
                                    /*countdown_ms=*/30000,
                                    /*heartbeat_ms=*/1000);

    ASSERT_TRUE(dms.start());
    ASSERT_TRUE(post->wait_for_calls(3, std::chrono::milliseconds(4000)));
    dms.stop();

    EXPECT_GE(post->call_count(), 3u)
        << "arm + at least two heartbeats expected in ~2.5s";
    for (const auto& [ep, body] : post->log)
    {
        EXPECT_EQ(ep, "/api/v3/trade/countdown-cancel-all");
        EXPECT_NE(body.find("\"countdown\":\"30\""), std::string::npos);
    }
}

TEST(BitgetFuturesDeadMansSwitch, LivenessTsAdvancesOnHeartbeat)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    BitgetFuturesDeadMansSwitch dms(wrap(post), 30000, 1000);

    ASSERT_TRUE(dms.start());

    const int64_t initial = dms.liveness_ts().load(std::memory_order_acquire);
    EXPECT_GT(initial, 0)
        << "initial arm should bump liveness for WorkerWatchdog";

    // Wait for a call observed strictly after the liveness snapshot. Under a
    // loaded monolithic run the first heartbeat may already have completed
    // before this test thread resumes; a fixed wait_for_calls(2) would then
    // return immediately and compare the same timestamp twice.
    const auto calls_after_snapshot = post->call_count();
    ASSERT_TRUE(post->wait_for_calls(
        calls_after_snapshot + 1, std::chrono::milliseconds(2500)));
    const auto publish_deadline = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds(500);
    int64_t later = dms.liveness_ts().load(std::memory_order_acquire);
    while (later <= initial
           && std::chrono::steady_clock::now() < publish_deadline)
    {
        std::this_thread::yield();
        later = dms.liveness_ts().load(std::memory_order_acquire);
    }
    EXPECT_GT(later, initial);

    dms.stop();
}

TEST(BitgetFuturesDeadMansSwitch, DisarmPostsCountdownZero)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    BitgetFuturesDeadMansSwitch dms(wrap(post), 30000, 1000);

    ASSERT_TRUE(dms.start());
    dms.stop();
    const auto before = post->call_count();

    EXPECT_TRUE(dms.disarm());
    ASSERT_EQ(post->call_count(), before + 1);
    EXPECT_NE(post->log.back().second.find("\"countdown\":\"0\""),
              std::string::npos);
}

TEST(BitgetFuturesDeadMansSwitch, StopJoinsCleanly)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    BitgetFuturesDeadMansSwitch dms(wrap(post), 30000, 1000);

    ASSERT_TRUE(dms.start());
    dms.stop();
    SUCCEED();
}

TEST(BitgetFuturesDeadMansSwitch, DoubleStopIsSafe)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    BitgetFuturesDeadMansSwitch dms(wrap(post), 30000, 1000);

    ASSERT_TRUE(dms.start());
    dms.stop();
    dms.stop();
    SUCCEED();
}

TEST(BitgetFuturesDeadMansSwitch, StopBeforeStartIsNoop)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    BitgetFuturesDeadMansSwitch dms(wrap(post), 30000, 1000);
    dms.stop();
    EXPECT_EQ(post->call_count(), 0u);
}

TEST(BitgetFuturesDeadMansSwitch, HeartbeatMinIs1000ms)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    // Operator asks for 50ms; venue rate limit forces ≥1000.
    BitgetFuturesDeadMansSwitch dms(wrap(post), 30000, /*heartbeat_ms=*/50);
    EXPECT_EQ(dms.heartbeat_interval_ms(), 1000);
}

TEST(BitgetFuturesDeadMansSwitch, Sub5sCountdownClampedWithWarn)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    BitgetFuturesDeadMansSwitch dms(wrap(post),
                                    /*countdown_ms=*/2500,
                                    /*heartbeat_ms=*/1000);
    EXPECT_EQ(dms.countdown_sec(), 5);
    EXPECT_NE(quiet.sink.str().find("clamping to 5s"), std::string::npos);
}

TEST(BitgetFuturesDeadMansSwitch, FirstFailureLatchesAndLateCallbackFires)
{
    SilenceStderr quiet;

    auto post = std::make_shared<fake_post>();
    // Initial arm OK, then every heartbeat fails.
    post->responses = {
        {200, R"({"code":"00000","msg":"success","data":"success"})"},
        {500, R"({"code":"50000","msg":"down"})"},
        {500, R"({"code":"50000","msg":"down"})"},
        {500, R"({"code":"50000","msg":"down"})"},
    };
    post->default_resp = {500, R"({"code":"50000","msg":"down"})"};

    BitgetFuturesDeadMansSwitch dms(
        wrap(post),
        /*countdown_ms=*/30000,
        /*heartbeat_ms=*/1000);

    ASSERT_TRUE(dms.start());

    ASSERT_TRUE(post->wait_for_calls(2, std::chrono::milliseconds(2500)));
    const auto latch_deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(500);
    while (!dms.failure_latched()
           && std::chrono::steady_clock::now() < latch_deadline)
        std::this_thread::yield();
    ASSERT_TRUE(dms.failure_latched());
    int callback_count = 0;
    dms.set_failure_callback([&](std::string_view) { ++callback_count; });
    dms.stop();

    EXPECT_EQ(post->call_count(), 2u);
    EXPECT_TRUE(dms.failure_latched());
    EXPECT_EQ(callback_count, 1);
}

TEST(BitgetFuturesDeadMansSwitch,
     LatchedHeartbeatFailureRefusesRestartWithoutPosting)
{
    SilenceStderr quiet;

    auto post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"00000","msg":"success","data":"success"})"},
        {500, R"({"code":"50000","msg":"down"})"},
    };

    BitgetFuturesDeadMansSwitch dms(
        wrap(post),
        /*countdown_ms=*/30000,
        /*heartbeat_ms=*/1000);
    ASSERT_TRUE(dms.start());

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(2500);
    while (!dms.failure_latched()
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();

    ASSERT_TRUE(dms.failure_latched());
    const auto calls_after_failure = post->call_count();

    EXPECT_FALSE(dms.start());
    EXPECT_EQ(post->call_count(), calls_after_failure)
        << "a terminally failed DMS must not re-arm the venue countdown";

    dms.stop();
    EXPECT_FALSE(dms.start());
    EXPECT_EQ(post->call_count(), calls_after_failure)
        << "joining the failed heartbeat thread must not clear the latch";
    EXPECT_NE(quiet.sink.str().find("refusing to restart"),
              std::string::npos);
}

TEST(BitgetFuturesDeadMansSwitch, BusinessCodeNot00000IsFailure)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"40001","msg":"invalid sign"})"},
    };
    BitgetFuturesDeadMansSwitch dms(wrap(post), 30000, 1000);
    EXPECT_FALSE(dms.start());
}

TEST(BitgetFuturesDeadMansSwitch, ThrowingHeartbeatLatchesWithoutTerminate)
{
    SilenceStderr quiet;
    std::atomic<int> calls{0};
    BitgetFuturesDeadMansSwitch dms(
        [&](std::string_view, std::string_view) -> response {
            if (calls.fetch_add(1, std::memory_order_acq_rel) == 0)
                return {200, R"({"code":"00000","msg":"success","data":"success"})"};
            throw 7;
        },
        5000, 1000);
    ASSERT_TRUE(dms.start());
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(2500);
    while (!dms.failure_latched()
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    EXPECT_TRUE(dms.failure_latched());
    dms.stop();
    EXPECT_EQ(calls.load(std::memory_order_acquire), 2);
}

#endif // HAS_BITGET
