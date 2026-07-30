#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_futures_dead_mans_switch.h"

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

using response = BitgetFuturesDeadMansSwitch::response;

struct fake_post
{
    response default_resp{200, R"({"code":"00000","msg":"success","data":"success"})"};
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
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
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

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const int64_t later = dms.liveness_ts().load(std::memory_order_acquire);
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

// Two consecutive HB cycle failures invoke close_position_fn once.
TEST(BitgetFuturesDeadMansSwitch, PersistentFailureInvokesCloseFn)
{
    SilenceStderr quiet;

    struct CloseRecorder
    {
        int call_count = 0;
        void operator()() { ++call_count; }
    };

    auto post = std::make_shared<fake_post>();
    // Initial arm OK, then every heartbeat fails.
    post->responses = {
        {200, R"({"code":"00000","msg":"success","data":"success"})"},
        {500, R"({"code":"50000","msg":"down"})"},
        {500, R"({"code":"50000","msg":"down"})"},
        {500, R"({"code":"50000","msg":"down"})"},
    };
    post->default_resp = {500, R"({"code":"50000","msg":"down"})"};

    CloseRecorder recorder;
    BitgetFuturesDeadMansSwitch dms(
        wrap(post),
        /*countdown_ms=*/30000,
        /*heartbeat_ms=*/1000,
        /*attempt_close=*/true,
        /*closer=*/[&recorder]() { recorder(); });

    ASSERT_TRUE(dms.start());

    // arm + wait for ≥2 failed heartbeat cycles (each 1000ms).
    std::this_thread::sleep_for(std::chrono::milliseconds(2800));
    dms.stop();

    EXPECT_GE(post->call_count(), 3u) << "arm + at least two failed HBs";
    EXPECT_EQ(recorder.call_count, 1)
        << "close fn must fire exactly once on 2 consecutive fails";
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

#endif // HAS_BITGET
