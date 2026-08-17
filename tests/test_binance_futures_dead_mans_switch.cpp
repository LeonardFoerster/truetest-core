#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_futures_dead_mans_switch.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
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
    response default_resp{200, ""};
    std::vector<response> responses;
    std::vector<std::pair<std::string, std::string>> log;
    std::mutex mu;
    std::condition_variable cv;

    response operator()(std::string_view ep, std::string_view p)
    {
        std::lock_guard<std::mutex> lk(mu);
        log.emplace_back(std::string(ep), std::string(p));
        auto response = log.size() <= responses.size()
            ? responses[log.size() - 1] : default_resp;
        if (response.body.empty())
        {
            const auto marker = p.find("countdownTime=");
            const auto value = marker == std::string_view::npos
                ? std::string_view{} : p.substr(marker + 14);
            response.body = "{\"symbol\":\"BTCUSDT\",\"countdownTime\":\""
                + std::string(value) + "\"}";
        }
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
    ASSERT_TRUE(post->wait_for_calls(5, std::chrono::milliseconds(1000)));
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

    ASSERT_TRUE(post->wait_for_calls(2, std::chrono::milliseconds(1000)));
    const int64_t later = dms.liveness_ts().load(std::memory_order_acquire);
    EXPECT_GT(later, initial)
        << "heartbeat must advance liveness so the watchdog stays happy";

    dms.stop();
}

TEST(BinanceFuturesDeadMansSwitch, FirstHeartbeatFailureLatchesAndStops)
{
    SilenceStderr quiet;

    auto post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"symbol":"BTCUSDT","countdownTime":"30000"})"},
        {500, "{\"msg\":\"down\"}"},
    };

    BinanceFuturesDeadMansSwitch dms(wrap(post), "BTCUSDT",
                                      /*countdown_ms=*/30000,
                                      /*heartbeat_ms=*/50);
    ASSERT_TRUE(dms.start());
    ASSERT_TRUE(post->wait_for_calls(2, std::chrono::milliseconds(500)));
    const auto latch_deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(500);
    while (!dms.failure_latched()
           && std::chrono::steady_clock::now() < latch_deadline)
        std::this_thread::yield();
    dms.stop();
    EXPECT_TRUE(dms.failure_latched());
    EXPECT_EQ(post->call_count(), 2u);
}

TEST(BinanceFuturesDeadMansSwitch,
     LatchedHeartbeatFailureRefusesRestartWithoutPosting)
{
    SilenceStderr quiet;

    auto post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"symbol":"BTCUSDT","countdownTime":"30000"})"},
        {500, "{\"msg\":\"down\"}"},
    };

    BinanceFuturesDeadMansSwitch dms(wrap(post), "BTCUSDT",
                                      /*countdown_ms=*/30000,
                                      /*heartbeat_ms=*/20);
    ASSERT_TRUE(dms.start());

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(500);
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

TEST(BinanceFuturesDeadMansSwitch, LateFailureCallbackReceivesLatchedFailure)
{
    SilenceStderr quiet;

    auto post = std::make_shared<fake_post>();
    // Initial arm succeeds, then every subsequent heartbeat fails.
    post->responses = {
        {200, R"({"symbol":"BTCUSDT","countdownTime":"30000"})"}, // initial arm
        {500, "{\"msg\":\"down\"}"},
        {500, "{\"msg\":\"down\"}"},
        {500, "{\"msg\":\"down\"}"},
    };
    post->default_resp = {500, "{\"msg\":\"down\"}"};

    BinanceFuturesDeadMansSwitch dms(
        wrap(post), "BTCUSDT",
        /*countdown_ms=*/30000,
        /*heartbeat_ms=*/20);

    ASSERT_TRUE(dms.start());

    ASSERT_TRUE(post->wait_for_calls(2, std::chrono::milliseconds(500)));
    const auto latch_deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(500);
    while (!dms.failure_latched()
           && std::chrono::steady_clock::now() < latch_deadline)
        std::this_thread::yield();
    ASSERT_TRUE(dms.failure_latched());
    int callback_count = 0;
    dms.set_failure_callback([&](std::string_view) { ++callback_count; });
    dms.stop();

    EXPECT_EQ(callback_count, 1);
}

TEST(BinanceFuturesDeadMansSwitch, MalformedSuccessRefusesInitialArm)
{
    SilenceStderr quiet;
    const std::string bodies[] = {
        "{}",
        "<html>ok</html>",
        R"({"symbol":"BTCUSDT","symbol":"ETHUSDT","countdownTime":"30000"})",
        "{\"symbol\":\"BTCUSDT\",\"countdownTime\":\"30000\"} trailing",
        R"({"symbol":"ETHUSDT","countdownTime":"30000"})",
        R"({"symbol":"BTCUSDT"})",
        R"({"nested":{"symbol":"BTCUSDT","countdownTime":"30000"}})",
        R"({"symbol":"BTCUSDT","countdownTime":"20000"})",
        R"({"code":200,"msg":"success"})",
    };
    for (const auto& body : bodies)
    {
        BinanceFuturesDeadMansSwitch dms(
            [body](std::string_view, std::string_view) {
                return response{200, body};
            },
            "BTCUSDT", 30000, 20);
        EXPECT_FALSE(dms.start()) << body;
    }
}

TEST(BinanceFuturesDeadMansSwitch, ThrowingHeartbeatLatchesWithoutTerminate)
{
    SilenceStderr quiet;
    std::atomic<int> calls{0};
    BinanceFuturesDeadMansSwitch dms(
        [&](std::string_view, std::string_view) -> response {
            if (calls.fetch_add(1, std::memory_order_acq_rel) == 0)
                return {200, R"({"symbol":"BTCUSDT","countdownTime":"30000"})"};
            throw 7;
        },
        "BTCUSDT", 30000, 10);
    ASSERT_TRUE(dms.start());
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(500);
    while (!dms.failure_latched()
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    EXPECT_TRUE(dms.failure_latched());
    dms.stop();
    EXPECT_EQ(calls.load(std::memory_order_acquire), 2);
}

#endif // HAS_BINANCE
