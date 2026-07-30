#include <gtest/gtest.h>

#ifdef HAS_GATE

#include "providers/gate/gate_futures_dead_mans_switch.h"

#include <atomic>
#include <chrono>
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

using response = GateFuturesDeadMansSwitch::response;

struct fake_post
{
    // Gate success is bare HTTP 2xx with optional trigger_time body.
    response default_resp{200, R"({"trigger_time":1710000030000})"};
    std::vector<response> responses;
    std::vector<std::pair<std::string, std::string>> log;
    std::mutex mu;

    response operator()(std::string_view ep, std::string_view body)
    {
        std::lock_guard<std::mutex> lk(mu);
        log.emplace_back(std::string(ep), std::string(body));
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
    return [f](std::string_view ep, std::string_view body) {
        return (*f)(ep, body);
    };
}

} // namespace

TEST(GateFuturesDeadMansSwitch, ClampCountdownSec)
{
    SilenceStderr quiet;
    // CLI ms → seconds: max(5, ms/1000)
    EXPECT_EQ(GateFuturesDeadMansSwitch::clamp_countdown_sec(40000), 40);
    EXPECT_EQ(GateFuturesDeadMansSwitch::clamp_countdown_sec(5000), 5);
    EXPECT_EQ(GateFuturesDeadMansSwitch::clamp_countdown_sec(30000), 30);
    EXPECT_EQ(GateFuturesDeadMansSwitch::clamp_countdown_sec(120000), 120);
    // Sub-5s configs WARN+clamp to 5.
    EXPECT_EQ(GateFuturesDeadMansSwitch::clamp_countdown_sec(3000), 5);
    EXPECT_EQ(GateFuturesDeadMansSwitch::clamp_countdown_sec(1), 5);
}

TEST(GateFuturesDeadMansSwitch, Sub5sCountdownClampedWithWarn)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    GateFuturesDeadMansSwitch dms(wrap(post), "BTC_USDT",
                                  /*countdown_ms=*/2500,
                                  /*heartbeat_ms=*/1000);
    EXPECT_EQ(dms.countdown_sec(), 5);
    EXPECT_EQ(dms.countdown_ms(), 5000);
    EXPECT_NE(quiet.sink.str().find("clamping to 5s"), std::string::npos);
}

TEST(GateFuturesDeadMansSwitch, ZeroHeartbeatUsesCountdownThird)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    GateFuturesDeadMansSwitch dms(wrap(post), "BTC_USDT",
                                  /*countdown_ms=*/30000,
                                  /*heartbeat_ms=*/0);
    EXPECT_EQ(dms.countdown_sec(), 30);
    EXPECT_EQ(dms.heartbeat_interval_ms(), 10000); // 30000/3
}

TEST(GateFuturesDeadMansSwitch, HeartbeatMinIs1000ms)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    GateFuturesDeadMansSwitch dms(wrap(post), "BTC_USDT", 30000,
                                  /*heartbeat_ms=*/50);
    EXPECT_EQ(dms.heartbeat_interval_ms(), 1000);
}

// Oversized HB reduced below countdown so refreshes can win the race.
TEST(GateFuturesDeadMansSwitch, OversizedHbReducedBelowTimer)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    GateFuturesDeadMansSwitch dms(wrap(post), "BTC_USDT",
                                  /*countdown_ms=*/15000,
                                  /*heartbeat_ms=*/20000);
    EXPECT_EQ(dms.countdown_sec(), 15);
    EXPECT_LT(dms.heartbeat_interval_ms(), 15000);
    EXPECT_EQ(dms.heartbeat_interval_ms(), 5000); // max(1000, 15000/3)
    EXPECT_NE(quiet.sink.str().find("reducing to"), std::string::npos);
}

TEST(GateFuturesDeadMansSwitch, StartArmsCountdown)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    GateFuturesDeadMansSwitch dms(wrap(post), "BTC_USDT",
                                  /*countdown_ms=*/30000,
                                  /*heartbeat_ms=*/1000);

    ASSERT_TRUE(dms.start());
    EXPECT_EQ(dms.countdown_sec(), 30);

    ASSERT_GE(post->call_count(), 1u);
    EXPECT_EQ(post->log[0].first,
              "/api/v4/futures/usdt/countdown_cancel_all");
    // timeout is a number in seconds, not ms and not a string.
    EXPECT_NE(post->log[0].second.find("\"timeout\":30"), std::string::npos);
    EXPECT_EQ(post->log[0].second.find("\"timeout\":30000"),
              std::string::npos)
        << "must never send ms as Gate timeout seconds";
    EXPECT_NE(post->log[0].second.find("\"contract\":\"BTC_USDT\""),
              std::string::npos);

    dms.stop();
}

TEST(GateFuturesDeadMansSwitch, StartFailsIfInitialArmFails)
{
    SilenceStderr quiet;

    auto post = std::make_shared<fake_post>();
    post->responses = {{500, R"({"label":"INTERNAL","message":"down"})"}};

    GateFuturesDeadMansSwitch dms(wrap(post), "BTC_USDT", 30000, 1000);
    EXPECT_FALSE(dms.start())
        << "initial arm failure must refuse start / live open";
    EXPECT_EQ(post->call_count(), 1u);
    EXPECT_NE(quiet.sink.str().find("refusing to start"), std::string::npos);
}

TEST(GateFuturesDeadMansSwitch, HeartbeatRefreshes)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    // heartbeat floor is 1000; wait ~2.5s for ≥2 beats beyond initial arm.
    GateFuturesDeadMansSwitch dms(wrap(post), "BTC_USDT",
                                  /*countdown_ms=*/30000,
                                  /*heartbeat_ms=*/1000);

    ASSERT_TRUE(dms.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    dms.stop();

    EXPECT_GE(post->call_count(), 3u)
        << "arm + at least two heartbeats expected in ~2.5s";
    for (const auto& [ep, body] : post->log)
    {
        EXPECT_EQ(ep, "/api/v4/futures/usdt/countdown_cancel_all");
        EXPECT_NE(body.find("\"timeout\":30"), std::string::npos);
        EXPECT_NE(body.find("\"contract\":\"BTC_USDT\""), std::string::npos);
    }
}

TEST(GateFuturesDeadMansSwitch, LivenessTsAdvancesOnHeartbeat)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    GateFuturesDeadMansSwitch dms(wrap(post), "BTC_USDT", 30000, 1000);

    ASSERT_TRUE(dms.start());

    const int64_t initial = dms.liveness_ts().load(std::memory_order_acquire);
    EXPECT_GT(initial, 0)
        << "initial arm should bump liveness for WorkerWatchdog";

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const int64_t later = dms.liveness_ts().load(std::memory_order_acquire);
    EXPECT_GT(later, initial);

    dms.stop();
}

TEST(GateFuturesDeadMansSwitch, DisarmPostsTimeoutZero)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    GateFuturesDeadMansSwitch dms(wrap(post), "BTC_USDT", 30000, 1000);

    ASSERT_TRUE(dms.start());
    dms.stop();
    const auto before = post->call_count();

    EXPECT_TRUE(dms.disarm());
    ASSERT_EQ(post->call_count(), before + 1);
    EXPECT_NE(post->log.back().second.find("\"timeout\":0"),
              std::string::npos);
    EXPECT_NE(post->log.back().second.find("\"contract\":\"BTC_USDT\""),
              std::string::npos);
}

TEST(GateFuturesDeadMansSwitch, StopJoinsCleanly)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    GateFuturesDeadMansSwitch dms(wrap(post), "BTC_USDT", 30000, 1000);

    ASSERT_TRUE(dms.start());
    dms.stop();
    SUCCEED();
}

TEST(GateFuturesDeadMansSwitch, DoubleStopIsSafe)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    GateFuturesDeadMansSwitch dms(wrap(post), "BTC_USDT", 30000, 1000);

    ASSERT_TRUE(dms.start());
    dms.stop();
    dms.stop();
    SUCCEED();
}

TEST(GateFuturesDeadMansSwitch, StopBeforeStartIsNoop)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    GateFuturesDeadMansSwitch dms(wrap(post), "BTC_USDT", 30000, 1000);
    dms.stop();
    EXPECT_EQ(post->call_count(), 0u);
}

// Two consecutive HB cycle failures invoke close_position_fn once.
TEST(GateFuturesDeadMansSwitch, PersistentFailureInvokesCloseFn)
{
    SilenceStderr quiet;

    struct CloseRecorder
    {
        int call_count = 0;
        std::string last_symbol;
        void operator()(const std::string& sym)
        {
            ++call_count;
            last_symbol = sym;
        }
    };

    auto post = std::make_shared<fake_post>();
    // Initial arm OK, then every heartbeat fails.
    post->responses = {
        {200, R"({"trigger_time":1})"},
        {500, R"({"label":"INTERNAL","message":"down"})"},
        {500, R"({"label":"INTERNAL","message":"down"})"},
        {500, R"({"label":"INTERNAL","message":"down"})"},
    };
    post->default_resp = {500, R"({"label":"INTERNAL","message":"down"})"};

    CloseRecorder recorder;
    GateFuturesDeadMansSwitch dms(
        wrap(post),
        "ETH_USDT",
        /*countdown_ms=*/30000,
        /*heartbeat_ms=*/1000,
        /*attempt_close=*/true,
        /*closer=*/[&recorder](const std::string& s) { recorder(s); });

    ASSERT_TRUE(dms.start());

    // arm + wait for ≥2 failed heartbeat cycles (each 1000ms).
    std::this_thread::sleep_for(std::chrono::milliseconds(2800));
    dms.stop();

    EXPECT_GE(post->call_count(), 3u) << "arm + at least two failed HBs";
    EXPECT_EQ(recorder.call_count, 1)
        << "close fn must fire exactly once on 2 consecutive fails";
    EXPECT_EQ(recorder.last_symbol, "ETH_USDT");
}

TEST(GateFuturesDeadMansSwitch, HttpNon2xxIsFailure)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    post->responses = {
        {400, R"({"label":"INVALID_PARAM_VALUE","message":"timeout too small"})"},
    };
    GateFuturesDeadMansSwitch dms(wrap(post), "BTC_USDT", 30000, 1000);
    EXPECT_FALSE(dms.start());
}

TEST(GateFuturesDeadMansSwitch, MsNeverSentAsTimeoutSeconds)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    // Classic footgun: 30000 ms must become timeout:30, not 30000.
    GateFuturesDeadMansSwitch dms(wrap(post), "BTC_USDT",
                                  /*countdown_ms=*/30000,
                                  /*heartbeat_ms=*/1000);
    ASSERT_TRUE(dms.start());
    ASSERT_GE(post->call_count(), 1u);
    const auto& body = post->log[0].second;
    EXPECT_NE(body.find("\"timeout\":30"), std::string::npos);
    EXPECT_EQ(body.find("\"timeout\":30000"), std::string::npos);
    dms.stop();
}

#endif // HAS_GATE
