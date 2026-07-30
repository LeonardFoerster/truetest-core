#include <gtest/gtest.h>

#ifdef HAS_GATE

#include "providers/gate/gate_futures_kill_switch.h"

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

using response = GateFuturesKillSwitch::response;

enum class verb
{
    del,
    get,
    post
};

struct call_record
{
    verb v;
    std::string path;
    std::string arg; // query for del/get, body for post
};

// Records each REST call and returns canned responses in order.
// Empty `responses` → default_resp for every call.
struct fake_rest
{
    response default_resp{200, R"([])"};
    std::vector<response> responses;
    std::vector<call_record> log;
    std::mutex mu;
    std::chrono::milliseconds sleep_on_call{0};

    response next(verb v, std::string_view path, std::string_view arg)
    {
        if (sleep_on_call.count() > 0)
            std::this_thread::sleep_for(sleep_on_call);

        std::lock_guard<std::mutex> lk(mu);
        log.push_back({v, std::string(path), std::string(arg)});
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

GateFuturesKillSwitch make_ks(
    std::shared_ptr<fake_rest> f,
    std::string symbol = "BTC_USDT",
    std::shared_ptr<ClientOrderIdMinter> minter = nullptr,
    GateFuturesKillSwitch::set_timeout_fn set_to = nullptr,
    GateFuturesKillSwitch::get_timeout_fn get_to = nullptr)
{
    auto del = [f](std::string_view path, std::string_view query) {
        return f->next(verb::del, path, query);
    };
    auto get = [f](std::string_view path, std::string_view query) {
        return f->next(verb::get, path, query);
    };
    auto post = [f](std::string_view path, std::string_view body) {
        return f->next(verb::post, path, body);
    };
    return GateFuturesKillSwitch(std::move(del), std::move(get),
                                 std::move(post), gate::usdt_mainnet(),
                                 std::move(symbol), std::move(minter),
                                 std::move(set_to), std::move(get_to));
}

// Happy-path canned sequence: cancel OK, flat position (skip flatten).
void set_flat_sequence(fake_rest& f)
{
    f.responses = {
        {200, R"([])"},                                    // cancel
        {200, R"({"contract":"BTC_USDT","size":0})"},      // position
    };
}

// Happy-path with non-zero long: cancel, pos +2, flatten -2.
void set_long_flatten_sequence(fake_rest& f)
{
    f.responses = {
        {200, R"([{"id":1,"contract":"BTC_USDT"}])"},
        {200, R"({"contract":"BTC_USDT","size":2})"},
        {200, R"({"id":99,"contract":"BTC_USDT","size":-2,"status":"finished"})"},
    };
}

} // namespace

TEST(GateFuturesKillSwitch, CallOrderCancelThenPositionThenFlatten)
{
    auto rest = std::make_shared<fake_rest>();
    set_long_flatten_sequence(*rest);
    auto ks = make_ks(rest);

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));

    ASSERT_EQ(rest->call_count(), 3u);
    EXPECT_EQ(rest->log[0].v, verb::del);
    EXPECT_EQ(rest->log[0].path, "/api/v4/futures/usdt/orders");
    EXPECT_EQ(rest->log[0].arg, "contract=BTC_USDT");

    EXPECT_EQ(rest->log[1].v, verb::get);
    EXPECT_EQ(rest->log[1].path, "/api/v4/futures/usdt/positions/BTC_USDT");

    EXPECT_EQ(rest->log[2].v, verb::post);
    EXPECT_EQ(rest->log[2].path, "/api/v4/futures/usdt/orders");
    // Opposite signed size, market ioc reduce-only.
    EXPECT_NE(rest->log[2].arg.find("\"size\":-2"), std::string::npos);
    EXPECT_NE(rest->log[2].arg.find("\"price\":\"0\""), std::string::npos);
    EXPECT_NE(rest->log[2].arg.find("\"tif\":\"ioc\""), std::string::npos);
    EXPECT_NE(rest->log[2].arg.find("\"reduce_only\":true"), std::string::npos);
    EXPECT_NE(rest->log[2].arg.find("\"contract\":\"BTC_USDT\""),
              std::string::npos);
    EXPECT_NE(rest->log[2].arg.find("\"text\":\"t-"), std::string::npos);
}

TEST(GateFuturesKillSwitch, FlatPositionSkipsFlatten)
{
    auto rest = std::make_shared<fake_rest>();
    set_flat_sequence(*rest);
    auto ks = make_ks(rest);

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    ASSERT_EQ(rest->call_count(), 2u);
    EXPECT_EQ(rest->log[0].v, verb::del);
    EXPECT_EQ(rest->log[1].v, verb::get);
}

TEST(GateFuturesKillSwitch, ShortPositionFlattensWithPositiveSize)
{
    auto rest = std::make_shared<fake_rest>();
    rest->responses = {
        {200, R"([])"},
        {200, R"({"contract":"ETH_USDT","size":-5})"},
        {200, R"({"id":7,"contract":"ETH_USDT","size":5})"},
    };
    auto ks = make_ks(rest, "ETH_USDT");

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    ASSERT_EQ(rest->call_count(), 3u);
    EXPECT_EQ(rest->log[1].path, "/api/v4/futures/usdt/positions/ETH_USDT");
    EXPECT_NE(rest->log[2].arg.find("\"size\":5"), std::string::npos);
    EXPECT_NE(rest->log[2].arg.find("\"contract\":\"ETH_USDT\""),
              std::string::npos);
}

TEST(GateFuturesKillSwitch, NoopCancelLabelStillProceedsToPosition)
{
    auto rest = std::make_shared<fake_rest>();
    rest->responses = {
        {404, R"({"label":"ORDER_NOT_FOUND","message":"Order not found"})"},
        {200, R"({"contract":"BTC_USDT","size":0})"},
    };
    auto ks = make_ks(rest);

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    ASSERT_EQ(rest->call_count(), 2u);
    EXPECT_EQ(rest->log[1].v, verb::get);
}

TEST(GateFuturesKillSwitch, NoOpenOrdersLabelIsOk)
{
    auto rest = std::make_shared<fake_rest>();
    rest->responses = {
        {400, R"({"label":"NO_OPEN_ORDERS","message":"no open orders"})"},
        {200, R"({"contract":"BTC_USDT","size":0})"},
    };
    auto ks = make_ks(rest);

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    EXPECT_EQ(rest->call_count(), 2u);
}

TEST(GateFuturesKillSwitch, PositionNotFoundIsFlat)
{
    auto rest = std::make_shared<fake_rest>();
    rest->responses = {
        {200, R"([])"},
        {404, R"({"label":"POSITION_NOT_FOUND","message":"Position not found"})"},
    };
    auto ks = make_ks(rest);

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    EXPECT_EQ(rest->call_count(), 2u); // no flatten
}

TEST(GateFuturesKillSwitch, HttpFailOnCancelReturnsFalse)
{
    SilenceStderr quiet;
    auto rest = std::make_shared<fake_rest>();
    rest->responses = {{500, R"({"label":"SERVER_ERROR","message":"boom"})"}};
    auto ks = make_ks(rest);

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    // Must not attempt position/flatten after cancel fail.
    EXPECT_EQ(rest->call_count(), 1u);
    EXPECT_EQ(rest->log[0].v, verb::del);
}

TEST(GateFuturesKillSwitch, HttpFailOnPositionReturnsFalse)
{
    SilenceStderr quiet;
    auto rest = std::make_shared<fake_rest>();
    rest->responses = {
        {200, R"([])"},
        {503, R"({"label":"SERVER_ERROR","message":"unavailable"})"},
    };
    auto ks = make_ks(rest);

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    EXPECT_EQ(rest->call_count(), 2u);
}

TEST(GateFuturesKillSwitch, HttpFailOnFlattenReturnsFalse)
{
    SilenceStderr quiet;
    auto rest = std::make_shared<fake_rest>();
    rest->responses = {
        {200, R"([])"},
        {200, R"({"contract":"BTC_USDT","size":1})"},
        {400, R"({"label":"INSUFFICIENT_AVAILABLE","message":"no margin"})"},
    };
    auto ks = make_ks(rest);

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    EXPECT_EQ(rest->call_count(), 3u);
}

TEST(GateFuturesKillSwitch, DeadlineExpiredAfterCancelSkipsPosition)
{
    SilenceStderr quiet;
    auto rest = std::make_shared<fake_rest>();
    // First call sleeps past the deadline; after cancel returns we check
    // wall clock and must refuse to call position/flatten.
    rest->sleep_on_call = std::chrono::milliseconds(80);
    rest->responses = {{200, R"([])"}};
    auto ks = make_ks(rest);

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(40)));
    EXPECT_EQ(rest->call_count(), 1u)
        << "position GET must not run after deadline expiry";
    EXPECT_EQ(rest->log[0].v, verb::del);
}

TEST(GateFuturesKillSwitch, NullRestFnsReturnFalse)
{
    SilenceStderr quiet;
    GateFuturesKillSwitch ks(nullptr, nullptr, nullptr, gate::usdt_mainnet(),
                             "BTC_USDT");
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(1000)));
}

TEST(GateFuturesKillSwitch, ImplementsIKillSwitchInterface)
{
    GateFuturesKillSwitch ks(nullptr, nullptr, nullptr, gate::usdt_mainnet(),
                             "BTC_USDT");
    IKillSwitch& base = ks;
    SilenceStderr quiet;
    EXPECT_FALSE(base.cancel_all_and_flatten(std::chrono::milliseconds(1)));
}

TEST(GateFuturesKillSwitch, NormalizesBareSymbolToUnderscore)
{
    auto rest = std::make_shared<fake_rest>();
    set_flat_sequence(*rest);
    // BTCUSDT → BTC_USDT (G2)
    auto ks = make_ks(rest, "BTCUSDT");

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    ASSERT_GE(rest->call_count(), 2u);
    EXPECT_EQ(rest->log[0].arg, "contract=BTC_USDT");
    EXPECT_EQ(rest->log[1].path, "/api/v4/futures/usdt/positions/BTC_USDT");
}

TEST(GateFuturesKillSwitch, SetsAndRestoresPerCallTimeout)
{
    auto rest = std::make_shared<fake_rest>();
    set_flat_sequence(*rest);

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

    auto ks = make_ks(rest, "BTC_USDT", nullptr, set_to, get_to);
    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(4500)));

    // tighten to min(1500, 1500)=1500, then restore 3000
    ASSERT_GE(set_log.size(), 2u);
    EXPECT_EQ(set_log.front(), 1500);
    EXPECT_EQ(set_log.back(), 3000);
    EXPECT_EQ(current_ms.load(), 3000);
}

TEST(GateFuturesKillSwitch, RestoresTimeoutAfterCancelFail)
{
    SilenceStderr quiet;
    auto rest = std::make_shared<fake_rest>();
    rest->responses = {{500, R"({"label":"SERVER_ERROR","message":"x"})"}};

    std::atomic<long long> current_ms{3000};
    auto set_to = [&](std::chrono::milliseconds ms) {
        current_ms.store(ms.count(), std::memory_order_release);
    };
    auto get_to = [&]() {
        return std::chrono::milliseconds(
            current_ms.load(std::memory_order_acquire));
    };

    auto ks = make_ks(rest, "BTC_USDT", nullptr, set_to, get_to);
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    EXPECT_EQ(current_ms.load(), 3000)
        << "timeout must restore even on early cancel failure";
}

TEST(GateFuturesKillSwitch, ExtractPositionSizeHelpers)
{
    double s = -1.0;
    EXPECT_TRUE(GateFuturesKillSwitch::extract_position_size(
        R"({"contract":"BTC_USDT","size":3})", s));
    EXPECT_DOUBLE_EQ(s, 3.0);

    EXPECT_TRUE(GateFuturesKillSwitch::extract_position_size(
        R"({"contract":"BTC_USDT","size":"-1.5"})", s));
    EXPECT_DOUBLE_EQ(s, -1.5);

    EXPECT_TRUE(GateFuturesKillSwitch::extract_position_size(
        R"({"label":"POSITION_NOT_FOUND","message":"x"})", s));
    EXPECT_DOUBLE_EQ(s, 0.0);

    EXPECT_TRUE(GateFuturesKillSwitch::extract_position_size(R"([])", s));
    EXPECT_DOUBLE_EQ(s, 0.0);

    EXPECT_TRUE(GateFuturesKillSwitch::extract_position_size(
        R"([{"contract":"BTC_USDT","size":0},{"contract":"ETH_USDT","size":4}])",
        s));
    EXPECT_DOUBLE_EQ(s, 4.0);

    // All-zero array → flat 0 (not refuse).
    EXPECT_TRUE(GateFuturesKillSwitch::extract_position_size(
        R"([{"contract":"BTC_USDT","size":0},{"contract":"ETH_USDT","size":0}])",
        s));
    EXPECT_DOUBLE_EQ(s, 0.0);

    // Leading whitespace before array still prefers non-zero row.
    EXPECT_TRUE(GateFuturesKillSwitch::extract_position_size(
        R"(  [{"contract":"BTC_USDT","size":0},{"contract":"ETH_USDT","size":2.5}])",
        s));
    EXPECT_DOUBLE_EQ(s, 2.5);

    EXPECT_FALSE(GateFuturesKillSwitch::extract_position_size(
        R"({"label":"SERVER_ERROR","message":"x"})", s));
}

TEST(GateFuturesKillSwitch, NoopLabelHelpers)
{
    EXPECT_TRUE(GateFuturesKillSwitch::is_cancel_noop_label("ORDER_NOT_FOUND"));
    EXPECT_TRUE(GateFuturesKillSwitch::is_cancel_noop_label("NO_OPEN_ORDERS"));
    EXPECT_TRUE(GateFuturesKillSwitch::is_cancel_noop_label("NO_CHANGE"));
    EXPECT_FALSE(GateFuturesKillSwitch::is_cancel_noop_label("SERVER_ERROR"));

    EXPECT_TRUE(
        GateFuturesKillSwitch::is_position_flat_label("POSITION_NOT_FOUND"));
    EXPECT_TRUE(GateFuturesKillSwitch::is_position_flat_label("NO_POSITION"));
    EXPECT_FALSE(GateFuturesKillSwitch::is_position_flat_label("ORDER_NOT_FOUND"));
}

TEST(GateFuturesKillSwitch, UsesMinterTextWithTPrefix)
{
    auto rest = std::make_shared<fake_rest>();
    set_long_flatten_sequence(*rest);
    // Deterministic minter: fixed epoch + seed.
    auto minter = std::make_shared<ClientOrderIdMinter>("tt", 0x42, 1000);
    auto ks = make_ks(rest, "BTC_USDT", minter);

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    ASSERT_EQ(rest->call_count(), 3u);
    // text must start with t-
    EXPECT_NE(rest->log[2].arg.find("\"text\":\"t-"), std::string::npos);
}

#endif // HAS_GATE
