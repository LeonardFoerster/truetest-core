#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_futures_kill_switch.h"

#include <atomic>
#include <chrono>
#include <iostream>
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

using response = BybitFuturesKillSwitch::response;

struct fake_io
{
    response default_post{200, R"({"retCode":0,"retMsg":"OK","result":{}})", 0, true};
    response default_get{
        200,
        R"({"retCode":0,"retMsg":"OK","result":{"list":[]}})",
        0, true};
    std::vector<response> post_responses;
    std::vector<response> get_responses;
    std::vector<std::pair<std::string, std::string>> post_log;
    std::vector<std::pair<std::string, std::string>> get_log;
    std::mutex mu;
    std::chrono::milliseconds sleep_on_post{0};

    response post(std::string_view ep, std::string_view body)
    {
        if (sleep_on_post.count() > 0)
            std::this_thread::sleep_for(sleep_on_post);

        std::lock_guard<std::mutex> lk(mu);
        post_log.emplace_back(std::string(ep), std::string(body));
        if (post_log.size() <= post_responses.size())
            return post_responses[post_log.size() - 1];
        return default_post;
    }

    response get(std::string_view ep, std::string_view q)
    {
        std::lock_guard<std::mutex> lk(mu);
        get_log.emplace_back(std::string(ep), std::string(q));
        if (get_log.size() <= get_responses.size())
            return get_responses[get_log.size() - 1];
        return default_get;
    }

    std::size_t post_count()
    {
        std::lock_guard<std::mutex> lk(mu);
        return post_log.size();
    }
    std::size_t get_count()
    {
        std::lock_guard<std::mutex> lk(mu);
        return get_log.size();
    }
};

std::shared_ptr<BybitFuturesKillSwitch>
make_ks(std::shared_ptr<fake_io> io,
        BybitFuturesKillSwitch::mint_fn mint = nullptr,
        BybitFuturesKillSwitch::set_timeout_fn set_to = nullptr,
        BybitFuturesKillSwitch::get_timeout_fn get_to = nullptr)
{
    return std::make_shared<BybitFuturesKillSwitch>(
        [io](std::string_view ep, std::string_view body) {
            return io->post(ep, body);
        },
        [io](std::string_view ep, std::string_view q) {
            return io->get(ep, q);
        },
        "BTCUSDT", "linear", std::move(mint),
        std::move(set_to), std::move(get_to));
}

std::string pos_body(const char* size, const char* side = "Buy")
{
    return std::string(R"({"retCode":0,"result":{"list":[{)")
         + R"("symbol":"BTCUSDT","side":")" + side
         + R"(","size":")" + size + R"(","positionIdx":0}]}})";
}

} // namespace

TEST(BybitFuturesKillSwitch, FlatPositionCancelOnlyThenGet)
{
    auto io = std::make_shared<fake_io>();
    auto ks = make_ks(io);

    EXPECT_TRUE(ks->cancel_all_and_flatten(std::chrono::milliseconds(3000)));

    ASSERT_EQ(io->post_count(), 1u);
    EXPECT_EQ(io->post_log[0].first, "/v5/order/cancel-all");
    EXPECT_NE(io->post_log[0].second.find("\"category\":\"linear\""),
              std::string::npos);
    EXPECT_NE(io->post_log[0].second.find("\"symbol\":\"BTCUSDT\""),
              std::string::npos);

    ASSERT_EQ(io->get_count(), 1u);
    EXPECT_EQ(io->get_log[0].first, "/v5/position/list");
}

TEST(BybitFuturesKillSwitch, LongPositionIssuesReduceOnlySell)
{
    auto io = std::make_shared<fake_io>();
    io->get_responses = {
        {200, pos_body("0.01", "Buy"), 0, true},
    };
    int mint_calls = 0;
    auto ks = make_ks(io, [&]() {
        ++mint_calls;
        return "tt-kill-1";
    });

    EXPECT_TRUE(ks->cancel_all_and_flatten(std::chrono::milliseconds(3000)));

    ASSERT_EQ(io->post_count(), 2u);
    EXPECT_EQ(io->post_log[0].first, "/v5/order/cancel-all");
    EXPECT_EQ(io->post_log[1].first, "/v5/order/create");
    EXPECT_NE(io->post_log[1].second.find("\"side\":\"Sell\""),
              std::string::npos);
    EXPECT_NE(io->post_log[1].second.find("\"reduceOnly\":true"),
              std::string::npos);
    EXPECT_NE(io->post_log[1].second.find("\"orderType\":\"Market\""),
              std::string::npos);
    EXPECT_NE(io->post_log[1].second.find("\"orderLinkId\":\"tt-kill-1\""),
              std::string::npos);
    EXPECT_EQ(mint_calls, 1);
}

TEST(BybitFuturesKillSwitch, ShortPositionIssuesReduceOnlyBuy)
{
    auto io = std::make_shared<fake_io>();
    io->get_responses = {
        {200, pos_body("1.5", "Sell"), 0, true},
    };
    auto ks = make_ks(io);

    EXPECT_TRUE(ks->cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    ASSERT_EQ(io->post_count(), 2u);
    EXPECT_NE(io->post_log[1].second.find("\"side\":\"Buy\""),
              std::string::npos);
    EXPECT_NE(io->post_log[1].second.find("\"qty\":\"1.5\""),
              std::string::npos);
}

TEST(BybitFuturesKillSwitch, NoopCancelCodeStillProceedsToPosition)
{
    auto io = std::make_shared<fake_io>();
    io->post_responses = {
        {200, R"({"retCode":110001,"retMsg":"order not exists"})", 110001, false},
    };
    auto ks = make_ks(io);

    EXPECT_TRUE(ks->cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    EXPECT_EQ(io->get_count(), 1u);
}

TEST(BybitFuturesKillSwitch, HttpFailOnCancelReturnsFalse)
{
    SilenceStderr quiet;
    auto io = std::make_shared<fake_io>();
    io->post_responses = {{500, R"({"retCode":10016,"retMsg":"server error"})", 10016, false}};
    auto ks = make_ks(io);

    EXPECT_FALSE(ks->cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    EXPECT_EQ(io->post_count(), 1u);
    EXPECT_EQ(io->get_count(), 0u);
}

TEST(BybitFuturesKillSwitch, PositionGetFailReturnsFalse)
{
    SilenceStderr quiet;
    auto io = std::make_shared<fake_io>();
    io->get_responses = {{503, "unavailable", -1, false}};
    auto ks = make_ks(io);

    EXPECT_FALSE(ks->cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    EXPECT_EQ(io->post_count(), 1u);
    EXPECT_EQ(io->get_count(), 1u);
}

TEST(BybitFuturesKillSwitch, FlattenCreateFailReturnsFalse)
{
    SilenceStderr quiet;
    auto io = std::make_shared<fake_io>();
    io->get_responses = {{200, pos_body("0.2", "Buy"), 0, true}};
    io->post_responses = {
        {200, R"({"retCode":0,"retMsg":"OK"})", 0, true},
        {200, R"({"retCode":110007,"retMsg":"insufficient"})", 110007, false},
    };
    auto ks = make_ks(io);

    EXPECT_FALSE(ks->cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    EXPECT_EQ(io->post_count(), 2u);
}

TEST(BybitFuturesKillSwitch, DeadlineExpiredAfterCancelSkipsFlatten)
{
    SilenceStderr quiet;
    auto io = std::make_shared<fake_io>();
    io->sleep_on_post = std::chrono::milliseconds(80);
    auto ks = make_ks(io);

    EXPECT_FALSE(ks->cancel_all_and_flatten(std::chrono::milliseconds(40)));
    EXPECT_EQ(io->post_count(), 1u);
    EXPECT_EQ(io->get_count(), 0u);
}

TEST(BybitFuturesKillSwitch, NullPostReturnsFalse)
{
    SilenceStderr quiet;
    BybitFuturesKillSwitch ks(nullptr, nullptr, "BTCUSDT");
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(1000)));
}

TEST(BybitFuturesKillSwitch, ImplementsIKillSwitchInterface)
{
    BybitFuturesKillSwitch ks(nullptr, nullptr, "BTCUSDT");
    IKillSwitch& base = ks;
    SilenceStderr quiet;
    EXPECT_FALSE(base.cancel_all_and_flatten(std::chrono::milliseconds(1)));
}

TEST(BybitFuturesKillSwitch, RestoresPreviousTimeoutAfterSuccess)
{
    auto io = std::make_shared<fake_io>();
    std::atomic<long long> current_ms{3000};
    std::vector<long long> set_log;
    auto set_to = [&](std::chrono::milliseconds ms) {
        current_ms.store(ms.count(), std::memory_order_release);
        set_log.push_back(ms.count());
    };
    auto get_to = [&]() {
        return std::chrono::milliseconds(
            current_ms.load(std::memory_order_acquire));
    };

    auto ks = make_ks(io, nullptr, set_to, get_to);
    EXPECT_TRUE(ks->cancel_all_and_flatten(std::chrono::milliseconds(4500)));

    ASSERT_GE(set_log.size(), 2u);
    EXPECT_EQ(set_log.front(), 1500);
    EXPECT_EQ(set_log.back(), 3000);
    EXPECT_EQ(current_ms.load(), 3000);
}

TEST(BybitFuturesKillSwitch, NoopCodeHelper)
{
    EXPECT_TRUE(BybitFuturesKillSwitch::is_cancel_noop_code("110001"));
    EXPECT_TRUE(BybitFuturesKillSwitch::is_cancel_noop_code("110010"));
    EXPECT_FALSE(BybitFuturesKillSwitch::is_cancel_noop_code("0"));
    EXPECT_FALSE(BybitFuturesKillSwitch::is_cancel_noop_code("10001"));
}

#endif // HAS_BYBIT
