#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_user_data_transport.h"

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace {

using namespace binance_keepalive_detail;

struct put_recorder
{
    std::vector<int>         statuses;
    std::vector<std::string> seen_keys;
    std::size_t              calls = 0;

    ka_response operator()(const std::string& key)
    {
        seen_keys.push_back(key);
        int s = calls < statuses.size() ? statuses[calls] : statuses.back();
        ++calls;
        return {s};
    }
};

struct post_recorder
{
    int status = 200;
    std::string new_key = "NEW_KEY";
    std::size_t calls = 0;

    ka_response operator()(std::string& out_key)
    {
        ++calls;
        out_key = new_key;
        return {status};
    }
};

auto zero_wait()
{
    return [](std::chrono::seconds, std::atomic<bool>& stop) {
        return stop.load();
    };
}

}

TEST(BinanceKeepalive, FirstAttemptSucceedsStaysOpen)
{
    binance_keepalive_policy pol;
    pol.max_retries = 3;
    pol.retry_delay = std::chrono::seconds(0);

    put_recorder put; put.statuses = {200};
    post_recorder post; post.status = 0;
    std::atomic<bool> stop{false};

    auto r = keepalive_tick(pol, "KEY", std::ref(put), std::ref(post), stop,
                            zero_wait());
    EXPECT_EQ(r.k, tick_result::kind::ok);
    EXPECT_EQ(put.calls, 1u);
    EXPECT_EQ(post.calls, 0u);
}

TEST(BinanceKeepalive, RecoversAfterTwoFailures)
{
    binance_keepalive_policy pol;
    pol.max_retries = 3;
    pol.retry_delay = std::chrono::seconds(0);

    put_recorder put; put.statuses = {500, 502, 200};
    post_recorder post; post.status = 0;
    std::atomic<bool> stop{false};

    auto r = keepalive_tick(pol, "KEY", std::ref(put), std::ref(post), stop,
                            zero_wait());
    EXPECT_EQ(r.k, tick_result::kind::ok);
    EXPECT_EQ(put.calls, 3u);
    EXPECT_EQ(post.calls, 0u);
}

TEST(BinanceKeepalive, RotatesWhenAllPutsFail)
{
    binance_keepalive_policy pol;
    pol.max_retries = 3;
    pol.retry_delay = std::chrono::seconds(0);

    put_recorder put; put.statuses = {500, 500, 500};
    post_recorder post; post.status = 200; post.new_key = "ROTATED";
    std::atomic<bool> stop{false};

    auto r = keepalive_tick(pol, "OLD", std::ref(put), std::ref(post), stop,
                            zero_wait());
    EXPECT_EQ(r.k, tick_result::kind::rotated);
    EXPECT_EQ(r.new_key, "ROTATED");
    EXPECT_EQ(put.calls, 3u);
    EXPECT_EQ(post.calls, 1u);
}

TEST(BinanceKeepalive, ErrorWhenRotationFails)
{
    binance_keepalive_policy pol;
    pol.max_retries = 2;
    pol.retry_delay = std::chrono::seconds(0);

    put_recorder put; put.statuses = {500, 500};
    post_recorder post; post.status = 500; post.new_key = "";
    std::atomic<bool> stop{false};

    auto r = keepalive_tick(pol, "OLD", std::ref(put), std::ref(post), stop,
                            zero_wait());
    EXPECT_EQ(r.k, tick_result::kind::error);
}

TEST(BinanceKeepalive, StopFlippedReturnsStopped)
{
    binance_keepalive_policy pol;
    pol.max_retries = 5;
    pol.retry_delay = std::chrono::seconds(60);

    put_recorder put; put.statuses = {500};
    post_recorder post; post.status = 200;
    std::atomic<bool> stop{false};

    auto wait_fn = [&stop](std::chrono::seconds,
                           std::atomic<bool>&) {
        stop.store(true);
        return true;
    };

    auto r = keepalive_tick(pol, "KEY", std::ref(put), std::ref(post), stop,
                            wait_fn);
    EXPECT_EQ(r.k, tick_result::kind::stopped);
}

#endif // HAS_BINANCE
