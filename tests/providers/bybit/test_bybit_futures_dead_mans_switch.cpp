#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_futures_dead_mans_switch.h"

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

using response = BybitFuturesDeadMansSwitch::response;

struct fake_health
{
    response default_resp{200, R"({"retCode":0,"retMsg":"OK","time":1700000000000})", true};
    std::vector<response> responses;
    std::size_t calls = 0;
    std::mutex mu;

    response operator()()
    {
        std::lock_guard<std::mutex> lk(mu);
        ++calls;
        if (calls <= responses.size())
            return responses[calls - 1];
        return default_resp;
    }

    std::size_t call_count()
    {
        std::lock_guard<std::mutex> lk(mu);
        return calls;
    }
};

} // namespace

TEST(BybitFuturesDeadMansSwitch, ClampCountdownSec)
{
    SilenceStderr quiet;
    EXPECT_EQ(BybitFuturesDeadMansSwitch::clamp_countdown_sec(40000), 40);
    EXPECT_EQ(BybitFuturesDeadMansSwitch::clamp_countdown_sec(5000), 5);
    EXPECT_EQ(BybitFuturesDeadMansSwitch::clamp_countdown_sec(60000), 60);
    EXPECT_EQ(BybitFuturesDeadMansSwitch::clamp_countdown_sec(300000), 300);
    EXPECT_EQ(BybitFuturesDeadMansSwitch::clamp_countdown_sec(400000), 300);
    EXPECT_EQ(BybitFuturesDeadMansSwitch::clamp_countdown_sec(3000), 5);
    EXPECT_EQ(BybitFuturesDeadMansSwitch::clamp_countdown_sec(1), 5);
}

TEST(BybitFuturesDeadMansSwitch, UpperCountdownClampWarns)
{
    SilenceStderr quiet;
    EXPECT_EQ(BybitFuturesDeadMansSwitch::clamp_countdown_sec(500000), 300);
    EXPECT_NE(quiet.sink.str().find("clamping to 300s"), std::string::npos);
}

TEST(BybitFuturesDeadMansSwitch, LargeCountdownClampsHbBelowTimer)
{
    SilenceStderr quiet;
    auto health = std::make_shared<fake_health>();
    // Countdown clamps to 300s; pass an HB that still exceeds the clamped
    // timer (operator set a huge heartbeat alongside a huge countdown).
    const int64_t raw_countdown = 500000; // clamps to 300s
    const int64_t raw_hb = 400000;        // ≥ 300000 after clamp → reduce
    BybitFuturesDeadMansSwitch dms(
        [health]() { return (*health)(); },
        raw_countdown, raw_hb);

    EXPECT_EQ(dms.countdown_sec(), 300);
    EXPECT_EQ(dms.countdown_ms(), 300000);
    EXPECT_LT(dms.heartbeat_interval_ms(), 300000);
    EXPECT_EQ(dms.heartbeat_interval_ms(), 100000); // max(1000, 300000/3)
    EXPECT_NE(quiet.sink.str().find("reducing to"), std::string::npos);
}

TEST(BybitFuturesDeadMansSwitch, ZeroHeartbeatUsesCountdownThird)
{
    SilenceStderr quiet;
    auto health = std::make_shared<fake_health>();
    BybitFuturesDeadMansSwitch dms(
        [health]() { return (*health)(); },
        /*countdown_ms=*/30000,
        /*heartbeat_ms=*/0);
    EXPECT_EQ(dms.countdown_sec(), 30);
    EXPECT_EQ(dms.heartbeat_interval_ms(), 10000);
}

TEST(BybitFuturesDeadMansSwitch, StartRequiresInitialHealth)
{
    SilenceStderr quiet;
    auto health = std::make_shared<fake_health>();
    BybitFuturesDeadMansSwitch dms(
        [health]() { return (*health)(); },
        30000, 1000);

    ASSERT_TRUE(dms.start());
    EXPECT_GE(health->call_count(), 1u);
    EXPECT_FALSE(dms.dcp_armed());
    dms.stop();
}

TEST(BybitFuturesDeadMansSwitch, StartFailsIfInitialHealthFails)
{
    SilenceStderr quiet;
    auto health = std::make_shared<fake_health>();
    health->responses = {{500, "err", false}};

    BybitFuturesDeadMansSwitch dms(
        [health]() { return (*health)(); },
        30000, 1000);
    EXPECT_FALSE(dms.start())
        << "initial health failure must refuse start / live open";
    EXPECT_EQ(health->call_count(), 1u);
}

TEST(BybitFuturesDeadMansSwitch, DcpArmFailureRefusesStart)
{
    SilenceStderr quiet;
    auto health = std::make_shared<fake_health>();
    int dcp_calls = 0;
    BybitFuturesDeadMansSwitch dms(
        [health]() { return (*health)(); },
        30000, 1000,
        /*attempt_close=*/false,
        /*closer=*/nullptr,
        /*dcp_arm=*/[&](int64_t /*sec*/) {
            ++dcp_calls;
            return false;
        });

    EXPECT_FALSE(dms.start());
    EXPECT_EQ(dcp_calls, 1);
    EXPECT_EQ(health->call_count(), 0u) << "health should not run if DCP fails";
}

TEST(BybitFuturesDeadMansSwitch, DcpArmSuccessThenHealth)
{
    SilenceStderr quiet;
    auto health = std::make_shared<fake_health>();
    int dcp_arm_sec = -1;
    BybitFuturesDeadMansSwitch dms(
        [health]() { return (*health)(); },
        30000, 1000,
        false, nullptr,
        [&](int64_t sec) {
            dcp_arm_sec = static_cast<int>(sec);
            return true;
        });

    ASSERT_TRUE(dms.start());
    EXPECT_EQ(dcp_arm_sec, 30);
    EXPECT_TRUE(dms.dcp_armed());
    EXPECT_TRUE(dms.disarm());
    dms.stop();
}

TEST(BybitFuturesDeadMansSwitch, LocalDisarmIsNoopSuccess)
{
    SilenceStderr quiet;
    auto health = std::make_shared<fake_health>();
    BybitFuturesDeadMansSwitch dms(
        [health]() { return (*health)(); }, 30000, 1000);
    ASSERT_TRUE(dms.start());
    EXPECT_TRUE(dms.disarm());
    dms.stop();
}

TEST(BybitFuturesDeadMansSwitch, TwoHbFailsInvokesCloserOnce)
{
    SilenceStderr quiet;
    auto health = std::make_shared<fake_health>();
    // First call (start) OK; subsequent fail.
    health->default_resp = {500, "down", false};
    health->responses = {
        {200, R"({"retCode":0,"time":1})", true},
    };

    std::atomic<int> close_calls{0};
    BybitFuturesDeadMansSwitch dms(
        [health]() { return (*health)(); },
        /*countdown_ms=*/9000,
        /*heartbeat_ms=*/50,
        /*attempt_close=*/true,
        /*closer=*/[&]() { close_calls.fetch_add(1); });

    ASSERT_TRUE(dms.start());
    // Wait for at least 2 failed heartbeats after start.
    for (int i = 0; i < 40 && close_calls.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_GE(close_calls.load(), 1);
    // Should not spam closer.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(close_calls.load(), 1);
    dms.stop();
}

TEST(BybitFuturesDeadMansSwitch, LocalOnlyCaveatLogged)
{
    SilenceStderr quiet;
    auto health = std::make_shared<fake_health>();
    BybitFuturesDeadMansSwitch dms(
        [health]() { return (*health)(); }, 30000, 1000);
    EXPECT_NE(quiet.sink.str().find("LOCAL DMS"), std::string::npos);
    EXPECT_NE(quiet.sink.str().find("NOT auto-cancelled"), std::string::npos);
}

#endif // HAS_BYBIT
