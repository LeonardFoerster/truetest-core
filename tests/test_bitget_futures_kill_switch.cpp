#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_futures_kill_switch.h"

#include <atomic>
#include <chrono>
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

using response = BitgetFuturesKillSwitch::response;

// Records each call and returns canned responses. Empty `responses` →
// default_resp for every call.
struct fake_post
{
    response default_resp{200, R"({"code":"00000","msg":"success","data":{}})"};
    std::vector<response> responses;
    std::vector<std::pair<std::string, std::string>> log;
    std::mutex mu;
    std::chrono::milliseconds sleep_on_call{0};

    response operator()(std::string_view ep, std::string_view body)
    {
        if (sleep_on_call.count() > 0)
            std::this_thread::sleep_for(sleep_on_call);

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

TEST(BitgetFuturesKillSwitch, CallOrderCancelThenClose)
{
    auto post = std::make_shared<fake_post>();
    BitgetFuturesKillSwitch ks(wrap(post), "USDT-FUTURES", "BTCUSDT");

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));

    ASSERT_EQ(post->call_count(), 2u);
    EXPECT_EQ(post->log[0].first, "/api/v3/trade/cancel-symbol-order");
    EXPECT_EQ(post->log[1].first, "/api/v3/trade/close-positions");

    // Bodies carry category + symbol.
    EXPECT_NE(post->log[0].second.find("\"category\":\"USDT-FUTURES\""),
              std::string::npos);
    EXPECT_NE(post->log[0].second.find("\"symbol\":\"BTCUSDT\""),
              std::string::npos);
    EXPECT_NE(post->log[1].second.find("\"category\":\"USDT-FUTURES\""),
              std::string::npos);
    EXPECT_NE(post->log[1].second.find("\"symbol\":\"BTCUSDT\""),
              std::string::npos);
}

TEST(BitgetFuturesKillSwitch, NoopCancelCodeStillProceedsToClose)
{
    auto post = std::make_shared<fake_post>();
    // Top-level "no orders" business code treated as cancel OK.
    post->responses = {
        {200, R"({"code":"25204","msg":"order does not exist"})"},
        {200, R"({"code":"00000","msg":"success","data":{}})"},
    };
    BitgetFuturesKillSwitch ks(wrap(post), "USDT-FUTURES", "BTCUSDT");

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    ASSERT_EQ(post->call_count(), 2u);
    EXPECT_EQ(post->log[1].first, "/api/v3/trade/close-positions");
}

TEST(BitgetFuturesKillSwitch, EmptyPositionCloseCodeIsOk)
{
    auto post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"00000","msg":"success"})"},
        {200, R"({"code":"25227","msg":"No position available to close"})"},
    };
    BitgetFuturesKillSwitch ks(wrap(post), "USDT-FUTURES", "ETHUSDT");

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    EXPECT_EQ(post->call_count(), 2u);
}

TEST(BitgetFuturesKillSwitch, HttpFailOnCancelReturnsFalse)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    post->responses = {{500, R"({"code":"50000","msg":"server error"})"}};
    BitgetFuturesKillSwitch ks(wrap(post), "USDT-FUTURES", "BTCUSDT");

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    // Must not attempt close after cancel fail.
    EXPECT_EQ(post->call_count(), 1u);
    EXPECT_EQ(post->log[0].first, "/api/v3/trade/cancel-symbol-order");
}

TEST(BitgetFuturesKillSwitch, HttpFailOnCloseReturnsFalse)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"00000","msg":"success"})"},
        {503, R"({"code":"50000","msg":"unavailable"})"},
    };
    BitgetFuturesKillSwitch ks(wrap(post), "USDT-FUTURES", "BTCUSDT");

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    EXPECT_EQ(post->call_count(), 2u);
}

TEST(BitgetFuturesKillSwitch, BusinessFailOnCancelReturnsFalse)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    // Unknown business code — not in the no-op allow-list.
    post->responses = {
        {200, R"({"code":"40762","msg":"balance insufficient"})"},
    };
    BitgetFuturesKillSwitch ks(wrap(post), "USDT-FUTURES", "BTCUSDT");

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    EXPECT_EQ(post->call_count(), 1u);
}

TEST(BitgetFuturesKillSwitch, DeadlineExpiredAfterCancelSkipsClose)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    // First call sleeps past the deadline; after cancel returns we check
    // wall clock and must refuse to call close.
    post->sleep_on_call = std::chrono::milliseconds(80);
    BitgetFuturesKillSwitch ks(wrap(post), "USDT-FUTURES", "BTCUSDT");

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(40)));
    EXPECT_EQ(post->call_count(), 1u)
        << "close-positions must not run after deadline expiry";
    EXPECT_EQ(post->log[0].first, "/api/v3/trade/cancel-symbol-order");
}

TEST(BitgetFuturesKillSwitch, NullPostReturnsFalse)
{
    SilenceStderr quiet;
    BitgetFuturesKillSwitch ks(nullptr, "USDT-FUTURES", "BTCUSDT");
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(1000)));
}

TEST(BitgetFuturesKillSwitch, ImplementsIKillSwitchInterface)
{
    BitgetFuturesKillSwitch ks(nullptr, "USDT-FUTURES", "BTCUSDT");
    IKillSwitch& base = ks;
    SilenceStderr quiet;
    EXPECT_FALSE(base.cancel_all_and_flatten(std::chrono::milliseconds(1)));
}

TEST(BitgetFuturesKillSwitch, SetsPerCallTimeoutWhenProvided)
{
    auto post = std::make_shared<fake_post>();
    std::atomic<long long> last_timeout_ms{-1};
    auto set_to = [&](std::chrono::milliseconds ms) {
        last_timeout_ms.store(ms.count(), std::memory_order_release);
    };

    BitgetFuturesKillSwitch ks(wrap(post), "USDT-FUTURES", "BTCUSDT", set_to);
    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(4500)));
    // No get_timeout → restore uses 0; final value after RAII restore.
    EXPECT_EQ(last_timeout_ms.load(), 0);
}

TEST(BitgetFuturesKillSwitch, RestoresPreviousTimeoutAfterSuccess)
{
    auto post = std::make_shared<fake_post>();
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

    BitgetFuturesKillSwitch ks(wrap(post), "USDT-FUTURES", "BTCUSDT",
                               set_to, get_to);
    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(4500)));

    // tighten to min(1500, 1500)=1500, then restore 3000
    ASSERT_GE(set_log.size(), 2u);
    EXPECT_EQ(set_log.front(), 1500);
    EXPECT_EQ(set_log.back(), 3000);
    EXPECT_EQ(current_ms.load(), 3000);
}

TEST(BitgetFuturesKillSwitch, RestoresPreviousTimeoutAfterCancelFail)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    post->responses = {{500, R"({"code":"50000","msg":"server error"})"}};

    std::atomic<long long> current_ms{3000};
    auto set_to = [&](std::chrono::milliseconds ms) {
        current_ms.store(ms.count(), std::memory_order_release);
    };
    auto get_to = [&]() {
        return std::chrono::milliseconds(
            current_ms.load(std::memory_order_acquire));
    };

    BitgetFuturesKillSwitch ks(wrap(post), "USDT-FUTURES", "BTCUSDT",
                               set_to, get_to);
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    EXPECT_EQ(current_ms.load(), 3000)
        << "timeout must restore even on early cancel failure";
}

TEST(BitgetFuturesKillSwitch, NoopCodeHelpers)
{
    EXPECT_TRUE(BitgetFuturesKillSwitch::is_cancel_noop_code("25204"));
    EXPECT_TRUE(BitgetFuturesKillSwitch::is_cancel_noop_code("22001"));
    EXPECT_FALSE(BitgetFuturesKillSwitch::is_cancel_noop_code("00000"));
    EXPECT_TRUE(BitgetFuturesKillSwitch::is_close_noop_code("25227"));
    EXPECT_TRUE(BitgetFuturesKillSwitch::is_close_noop_code("22002"));
    EXPECT_FALSE(BitgetFuturesKillSwitch::is_close_noop_code("40762"));
}

#endif // HAS_BITGET
