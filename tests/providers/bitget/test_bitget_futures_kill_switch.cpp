#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_futures_kill_switch.h"

#include <algorithm>
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
    response default_resp{200, R"({"code":"00000","msg":"success","data":{"list":[]}})"};
    std::vector<response> responses;
    std::vector<std::pair<std::string, std::string>> log;
    std::mutex mu;
    std::chrono::milliseconds sleep_on_call{0};
    std::vector<std::chrono::milliseconds> deadlines;

    response operator()(std::string_view ep, std::string_view body,
                        std::chrono::milliseconds deadline)
    {
        if (sleep_on_call.count() > 0)
            std::this_thread::sleep_for(sleep_on_call);

        std::lock_guard<std::mutex> lk(mu);
        log.emplace_back(std::string(ep), std::string(body));
        deadlines.push_back(deadline);
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

BitgetFuturesKillSwitch::post_fn
wrap(std::shared_ptr<fake_post> f)
{
    return [f](std::string_view ep, std::string_view body,
               std::chrono::milliseconds deadline) {
        return (*f)(ep, body, deadline);
    };
}

response empty_regular_orders()
{
    return {200,
        R"({"code":"00000","msg":"success","data":{"list":[],"cursor":""}})"};
}

BitgetFuturesKillSwitch::get_fn position_readback(std::string total = "0")
{
    return [total = std::move(total)](std::string_view endpoint,
                                     std::string_view,
              std::chrono::milliseconds) -> response {
        if (endpoint == "/api/v3/trade/unfilled-strategy-orders")
            return {200, R"({"code":"00000","msg":"success","data":[]})"};
        if (endpoint == "/api/v3/trade/unfilled-orders")
            return empty_regular_orders();
        return {200, "{\"code\":\"00000\",\"msg\":\"success\",\"data\":{\"list\":[{\"symbol\":\"BTCUSDT\",\"total\":\""
            + total + "\"}]}}"};
    };
}

} // namespace

TEST(BitgetFuturesKillSwitch, CallOrderCancelThenClose)
{
    auto post = std::make_shared<fake_post>();
    BitgetFuturesKillSwitch ks(
        wrap(post), "USDT-FUTURES", "BTCUSDT", position_readback());

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

TEST(BitgetFuturesKillSwitch, SweepsStrategyOrdersAndProvesEmptyReadback)
{
    std::vector<std::string> post_endpoints;
    bool strategy_cancelled = false;
    BitgetFuturesKillSwitch::post_fn post =
        [&](std::string_view endpoint, std::string_view body,
            std::chrono::milliseconds) -> response {
        post_endpoints.emplace_back(endpoint);
        if (endpoint == "/api/v3/trade/cancel-strategy-order")
        {
            EXPECT_NE(body.find("\"orderId\":\"91\""),
                      std::string_view::npos);
            strategy_cancelled = true;
            return {200, R"({"code":"00000","msg":"success","data":null})"};
        }
        return {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"};
    };
    BitgetFuturesKillSwitch::get_fn get =
        [&](std::string_view endpoint, std::string_view query,
            std::chrono::milliseconds) -> response {
        if (endpoint == "/api/v3/position/current-position")
            return {200, R"({"code":"00000","msg":"success","data":{"list":[{"symbol":"BTCUSDT","total":"0"}]}})"};
        if (endpoint == "/api/v3/trade/unfilled-orders")
            return empty_regular_orders();
        if (query.find("type=tpsl") != std::string_view::npos
            && !strategy_cancelled)
            return {200, R"({"code":"00000","msg":"success","data":[{"orderId":"91","clientOid":"tt-fb-7","category":"USDT-FUTURES","symbol":"BTCUSDT","status":"pending"}]})"};
        if (query.find("type=trigger") != std::string_view::npos)
            return {200, R"({"code":"00000","msg":"success","data":[{"orderId":"92","clientOid":"foreign","category":"USDT-FUTURES","symbol":"ETHUSDT","status":"pending"}]})"};
        return {200, R"({"code":"00000","msg":"success","data":[]})"};
    };
    BitgetFuturesKillSwitch ks(
        std::move(post), "USDT-FUTURES", "BTCUSDT", std::move(get));

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    ASSERT_EQ(post_endpoints.size(), 3u);
    EXPECT_EQ(post_endpoints[0], "/api/v3/trade/cancel-symbol-order");
    EXPECT_EQ(post_endpoints[1], "/api/v3/trade/cancel-strategy-order");
    EXPECT_EQ(post_endpoints[2], "/api/v3/trade/close-positions");
}

TEST(BitgetFuturesKillSwitch, ResidualRegularOrderKeepsKillFailedButStillCloses)
{
    SilenceStderr quiet;
    bool close_called = false;
    BitgetFuturesKillSwitch::post_fn post =
        [&](std::string_view endpoint, std::string_view,
            std::chrono::milliseconds) -> response {
        if (endpoint == "/api/v3/trade/close-positions")
            close_called = true;
        return {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"};
    };
    BitgetFuturesKillSwitch::get_fn get =
        [](std::string_view endpoint, std::string_view,
           std::chrono::milliseconds) -> response {
        if (endpoint == "/api/v3/trade/unfilled-orders")
            return {200, R"({"code":"00000","msg":"success","data":{"list":[{"orderId":"91","clientOid":"tt-91","category":"USDT-FUTURES","symbol":"BTCUSDT","orderStatus":"live"}],"cursor":"91"}})"};
        if (endpoint == "/api/v3/position/current-position")
            return {200, R"({"code":"00000","msg":"success","data":{"list":[{"symbol":"BTCUSDT","total":"0"}]}})"};
        return {200, R"({"code":"00000","msg":"success","data":[]})"};
    };
    BitgetFuturesKillSwitch ks(
        std::move(post), "USDT-FUTURES", "BTCUSDT", std::move(get));

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_TRUE(close_called);
}

TEST(BitgetFuturesKillSwitch, RegularOrderReadbackRequiresScopedAuthoritativeEmptyList)
{
    SilenceStderr quiet;
    const std::vector<response> invalid_readbacks{
        {500, R"({"code":"50000","msg":"error"})"},
        {200, R"({"code":"50000","msg":"error","data":{"list":[]}})"},
        {200, R"({"code":"00000","msg":"success","data":{}})"},
        {200, R"({"code":"00000","msg":"success","data":{"list":{}}})"},
    };

    for (const auto& regular_response : invalid_readbacks)
    {
        bool close_called = false;
        bool scoped_query_seen = false;
        BitgetFuturesKillSwitch::post_fn post =
            [&](std::string_view endpoint, std::string_view,
                std::chrono::milliseconds) -> response {
            if (endpoint == "/api/v3/trade/close-positions")
                close_called = true;
            return {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"};
        };
        BitgetFuturesKillSwitch::get_fn get =
            [&](std::string_view endpoint, std::string_view query,
                std::chrono::milliseconds) -> response {
            if (endpoint == "/api/v3/trade/unfilled-orders")
            {
                scoped_query_seen =
                    query == "category=USDT-FUTURES&symbol=BTCUSDT";
                return regular_response;
            }
            if (endpoint == "/api/v3/position/current-position")
                return {200, R"({"code":"00000","msg":"success","data":{"list":[{"symbol":"BTCUSDT","total":"0"}]}})"};
            return {200, R"({"code":"00000","msg":"success","data":[]})"};
        };
        BitgetFuturesKillSwitch ks(
            std::move(post), "USDT-FUTURES", "BTCUSDT", std::move(get));

        EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
        EXPECT_TRUE(scoped_query_seen);
        EXPECT_TRUE(close_called);
    }
}

TEST(BitgetFuturesKillSwitch, ThrowingRegularReadbackStillSweepsAndCloses)
{
    SilenceStderr quiet;
    bool close_called = false;
    int strategy_reads = 0;
    BitgetFuturesKillSwitch::post_fn post =
        [&](std::string_view endpoint, std::string_view,
            std::chrono::milliseconds) -> response {
        if (endpoint == "/api/v3/trade/close-positions")
            close_called = true;
        return {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"};
    };
    BitgetFuturesKillSwitch::get_fn get =
        [&](std::string_view endpoint, std::string_view,
            std::chrono::milliseconds) -> response {
        if (endpoint == "/api/v3/trade/unfilled-orders")
            throw std::runtime_error("regular readback failed");
        if (endpoint == "/api/v3/trade/unfilled-strategy-orders")
        {
            ++strategy_reads;
            return {200, R"({"code":"00000","msg":"success","data":[]})"};
        }
        return {200, R"({"code":"00000","msg":"success","data":{"list":[{"symbol":"BTCUSDT","total":"0"}]}})"};
    };
    BitgetFuturesKillSwitch ks(
        std::move(post), "USDT-FUTURES", "BTCUSDT", std::move(get));

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_EQ(strategy_reads, 4);
    EXPECT_TRUE(close_called);
}

TEST(BitgetFuturesKillSwitch, ThrowingTpslListStillSweepsTriggerAndCloses)
{
    SilenceStderr quiet;
    bool close_called = false;
    int trigger_reads = 0;
    BitgetFuturesKillSwitch::post_fn post =
        [&](std::string_view endpoint, std::string_view,
            std::chrono::milliseconds) -> response {
        if (endpoint == "/api/v3/trade/close-positions")
            close_called = true;
        return {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"};
    };
    BitgetFuturesKillSwitch::get_fn get =
        [&](std::string_view endpoint, std::string_view query,
            std::chrono::milliseconds) -> response {
        if (endpoint == "/api/v3/trade/unfilled-orders")
            return empty_regular_orders();
        if (endpoint == "/api/v3/trade/unfilled-strategy-orders")
        {
            if (query.find("type=tpsl") != std::string_view::npos)
                throw std::runtime_error("tpsl list failed");
            ++trigger_reads;
            return {200, R"({"code":"00000","msg":"success","data":[]})"};
        }
        return {200, R"({"code":"00000","msg":"success","data":{"list":[{"symbol":"BTCUSDT","total":"0"}]}})"};
    };
    BitgetFuturesKillSwitch ks(
        std::move(post), "USDT-FUTURES", "BTCUSDT", std::move(get));

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_EQ(trigger_reads, 2);
    EXPECT_TRUE(close_called);
}

TEST(BitgetFuturesKillSwitch, ResidualStrategyOrderKeepsKillFailedButStillCloses)
{
    SilenceStderr quiet;
    std::vector<std::string> posts;
    BitgetFuturesKillSwitch::post_fn post =
        [&](std::string_view endpoint, std::string_view,
            std::chrono::milliseconds) -> response {
        posts.emplace_back(endpoint);
        if (endpoint == "/api/v3/trade/cancel-strategy-order")
            return {200, R"({"code":"00000","msg":"success","data":null})"};
        return {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"};
    };
    BitgetFuturesKillSwitch::get_fn get =
        [](std::string_view endpoint, std::string_view query,
           std::chrono::milliseconds) -> response {
        if (endpoint == "/api/v3/position/current-position")
            return {200, R"({"code":"00000","msg":"success","data":{"list":[{"symbol":"BTCUSDT","total":"0"}]}})"};
        if (endpoint == "/api/v3/trade/unfilled-orders")
            return empty_regular_orders();
        if (query.find("type=tpsl") != std::string_view::npos)
            return {200, R"({"code":"00000","msg":"success","data":[{"orderId":"91","clientOid":"tt-fb-7","category":"USDT-FUTURES","symbol":"BTCUSDT","status":"pending"}]})"};
        return {200, R"({"code":"00000","msg":"success","data":[]})"};
    };
    BitgetFuturesKillSwitch ks(
        std::move(post), "USDT-FUTURES", "BTCUSDT", std::move(get));

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_NE(std::find(posts.begin(), posts.end(),
                        "/api/v3/trade/cancel-strategy-order"), posts.end());
    EXPECT_NE(std::find(posts.begin(), posts.end(),
                        "/api/v3/trade/close-positions"), posts.end());
}

TEST(BitgetFuturesKillSwitch, ThrowingStrategyCancelStillReadsBackAndCloses)
{
    SilenceStderr quiet;
    bool first_list = true;
    bool close_called = false;
    BitgetFuturesKillSwitch::post_fn post =
        [&](std::string_view endpoint, std::string_view,
            std::chrono::milliseconds) -> response {
        if (endpoint == "/api/v3/trade/cancel-strategy-order")
            throw std::runtime_error("strategy cancel failed");
        if (endpoint == "/api/v3/trade/close-positions")
            close_called = true;
        return {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"};
    };
    BitgetFuturesKillSwitch::get_fn get =
        [&](std::string_view endpoint, std::string_view query,
            std::chrono::milliseconds) -> response {
        if (endpoint == "/api/v3/position/current-position")
            return {200, R"({"code":"00000","msg":"success","data":{"list":[{"symbol":"BTCUSDT","total":"0"}]}})"};
        if (endpoint == "/api/v3/trade/unfilled-orders")
            return empty_regular_orders();
        if (query.find("type=tpsl") != std::string_view::npos && first_list)
        {
            first_list = false;
            return {200, R"({"code":"00000","msg":"success","data":[{"orderId":"91","clientOid":"tt-fb-7","category":"USDT-FUTURES","symbol":"BTCUSDT","status":"pending"}]})"};
        }
        return {200, R"({"code":"00000","msg":"success","data":[]})"};
    };
    BitgetFuturesKillSwitch ks(
        std::move(post), "USDT-FUTURES", "BTCUSDT", std::move(get));

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_TRUE(close_called);
}

TEST(BitgetFuturesKillSwitch, MalformedStrategyListFailsClosedAndStillFlattens)
{
    SilenceStderr quiet;
    bool close_called = false;
    BitgetFuturesKillSwitch::post_fn post =
        [&](std::string_view endpoint, std::string_view,
            std::chrono::milliseconds) -> response {
        if (endpoint == "/api/v3/trade/close-positions")
            close_called = true;
        return {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"};
    };
    BitgetFuturesKillSwitch::get_fn get =
        [](std::string_view endpoint, std::string_view query,
           std::chrono::milliseconds) -> response {
        if (endpoint == "/api/v3/position/current-position")
            return {200, R"({"code":"00000","msg":"success","data":{"list":[{"symbol":"BTCUSDT","total":"0"}]}})"};
        if (endpoint == "/api/v3/trade/unfilled-orders")
            return empty_regular_orders();
        if (query.find("type=tpsl") != std::string_view::npos)
            return {200, R"({"code":"00000","msg":"success","data":[{"orderId":"91","orderId":"92","clientOid":"tt-fb-7","category":"USDT-FUTURES","symbol":"BTCUSDT","status":"pending"}]})"};
        return {200, R"({"code":"00000","msg":"success","data":[]})"};
    };
    BitgetFuturesKillSwitch ks(
        std::move(post), "USDT-FUTURES", "BTCUSDT", std::move(get));

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_TRUE(close_called);
}

TEST(BitgetFuturesKillSwitch, AcceptsDocumentedUtaV3BatchResponses)
{
    auto post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"00000","msg":"success","requestTime":1750408713175,"data":{"list":[{"orderId":"123456","clientOid":"tt-cancel","code":"24056","msg":"notExisted"}]}})"},
        {200, R"({"code":"00000","msg":"success","requestTime":1750408713176,"data":{"list":[{"orderId":"123457","clientOid":"tt-close","code":"00000","msg":"success"}]}})"},
    };
    BitgetFuturesKillSwitch ks(
        wrap(post), "USDT-FUTURES", "BTCUSDT",
        position_readback());

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_EQ(post->call_count(), 2u);
}

TEST(BitgetFuturesKillSwitch, CloseMutationRequiresFlatPositionReadback)
{
    SilenceStderr quiet;
    auto make_post = [] {
        auto post = std::make_shared<fake_post>();
        post->responses = {
            {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"},
            {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"},
        };
        return post;
    };

    auto post = make_post();
    BitgetFuturesKillSwitch flat(
        wrap(post), "USDT-FUTURES", "BTCUSDT",
        position_readback());
    EXPECT_TRUE(flat.cancel_all_and_flatten(std::chrono::seconds(1)));

    post = make_post();
    BitgetFuturesKillSwitch exposed(
        wrap(post), "USDT-FUTURES", "BTCUSDT",
        position_readback("1"));
    EXPECT_FALSE(exposed.cancel_all_and_flatten(std::chrono::seconds(1)));

    post = make_post();
    BitgetFuturesKillSwitch unverifiable(
        wrap(post), "USDT-FUTURES", "BTCUSDT");
    EXPECT_FALSE(unverifiable.cancel_all_and_flatten(
        std::chrono::seconds(1)));
}

TEST(BitgetFuturesKillSwitch, NoopCancelCodeStillProceedsToClose)
{
    auto post = std::make_shared<fake_post>();
    // Top-level "no orders" business code treated as cancel OK.
    post->responses = {
        {200, R"({"code":"25204","msg":"order does not exist"})"},
        {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"},
    };
    BitgetFuturesKillSwitch ks(
        wrap(post), "USDT-FUTURES", "BTCUSDT", position_readback());

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    ASSERT_EQ(post->call_count(), 2u);
    EXPECT_EQ(post->log[1].first, "/api/v3/trade/close-positions");
}

TEST(BitgetFuturesKillSwitch, EmptyPositionCloseCodeIsOk)
{
    auto post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"},
        {200, R"({"code":"25227","msg":"No position available to close"})"},
    };
    BitgetFuturesKillSwitch ks(
        wrap(post), "USDT-FUTURES", "ETHUSDT", position_readback());

    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    EXPECT_EQ(post->call_count(), 2u);
}

TEST(BitgetFuturesKillSwitch, HttpFailOnCancelReturnsFalse)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    post->responses = {{500, R"({"code":"50000","msg":"server error"})"}};
    BitgetFuturesKillSwitch ks(
        wrap(post), "USDT-FUTURES", "BTCUSDT", position_readback());

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(3000)));
    // Flatten is an independent risk-reducing action; overall result remains
    // false, but it is still attempted within the shared deadline.
    EXPECT_EQ(post->call_count(), 2u);
    EXPECT_EQ(post->log[0].first, "/api/v3/trade/cancel-symbol-order");
}

TEST(BitgetFuturesKillSwitch, ThrowingCancelStillAttemptsClose)
{
    SilenceStderr quiet;
    int calls = 0;
    BitgetFuturesKillSwitch ks(
        [&](std::string_view, std::string_view,
            std::chrono::milliseconds) -> response {
            if (++calls == 1) throw std::runtime_error("cancel failed");
            return {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"};
        },
        "USDT-FUTURES", "BTCUSDT");
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_EQ(calls, 2);
}

TEST(BitgetFuturesKillSwitch, ThrowingCloseReturnsFalse)
{
    SilenceStderr quiet;
    int close_calls = 0;
    BitgetFuturesKillSwitch ks(
        [&](std::string_view endpoint, std::string_view,
            std::chrono::milliseconds) -> response {
            if (endpoint == "/api/v3/trade/close-positions")
            {
                ++close_calls;
                throw std::runtime_error("close failed");
            }
            return {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"};
        },
        "USDT-FUTURES", "BTCUSDT", position_readback());

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_EQ(close_calls, 1);
}

TEST(BitgetFuturesKillSwitch, ThrowingFlatReadbackReturnsFalse)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    BitgetFuturesKillSwitch::get_fn get =
        [](std::string_view endpoint, std::string_view,
           std::chrono::milliseconds) -> response {
        if (endpoint == "/api/v3/trade/unfilled-orders")
            return empty_regular_orders();
        if (endpoint == "/api/v3/trade/unfilled-strategy-orders")
            return {200, R"({"code":"00000","msg":"success","data":[]})"};
        if (endpoint == "/api/v3/position/current-position")
            throw 7;
        return {};
    };
    BitgetFuturesKillSwitch ks(
        wrap(post), "USDT-FUTURES", "BTCUSDT", std::move(get));

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_EQ(post->call_count(), 2u);
}

TEST(BitgetFuturesKillSwitch, DuplicateBusinessCodeNeverCountsAsNoop)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"25204","code":"00000"})"},
        {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"},
    };
    BitgetFuturesKillSwitch ks(
        wrap(post), "USDT-FUTURES", "BTCUSDT", position_readback());
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_EQ(post->call_count(), 2u);
}

TEST(BitgetFuturesKillSwitch, MalformedOrPartialBatchSuccessNeverDisarms)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"00000","data":{}})"},
        {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"},
    };
    BitgetFuturesKillSwitch malformed(
        wrap(post), "USDT-FUTURES", "BTCUSDT");
    EXPECT_FALSE(malformed.cancel_all_and_flatten(std::chrono::seconds(1)));

    post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"00000","msg":"success","data":{"list":[{"orderId":"7","clientOid":"tt-7","code":"40000","msg":"failed"}]}})"},
        {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"},
    };
    BitgetFuturesKillSwitch partial(
        wrap(post), "USDT-FUTURES", "BTCUSDT");
    EXPECT_FALSE(partial.cancel_all_and_flatten(std::chrono::seconds(1)));

    post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"00000","msg":"success","data":{"list":[{"orderId":"7","clientOid":"tt-7","code":"00000","msg":"wrong"}]}})"},
        {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"},
    };
    BitgetFuturesKillSwitch wrong_symbol(
        wrap(post), "USDT-FUTURES", "BTCUSDT");
    EXPECT_FALSE(wrong_symbol.cancel_all_and_flatten(std::chrono::seconds(1)));

    post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"00000","msg":"success","data":{"list":[{"nested":{"orderId":"7","clientOid":"tt-7","code":"00000","msg":"success"}}]}})"},
        {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"},
    };
    BitgetFuturesKillSwitch nested_only(
        wrap(post), "USDT-FUTURES", "BTCUSDT");
    EXPECT_FALSE(nested_only.cancel_all_and_flatten(std::chrono::seconds(1)));
}

TEST(BitgetFuturesKillSwitch, BatchRowsRequireIdentityAndEndpointStatus)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"00000","msg":"success","data":{"list":[{"orderId":"","clientOid":"","code":"00000","msg":"success"}]}})"},
        {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"},
    };
    BitgetFuturesKillSwitch missing_identity(
        wrap(post), "USDT-FUTURES", "BTCUSDT");
    EXPECT_FALSE(missing_identity.cancel_all_and_flatten(
        std::chrono::seconds(1)));

    post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"00000","msg":"success","data":{"list":[{"orderId":"7","clientOid":"tt-7","code":"00000","code":"24056","msg":"notExisted"}]}})"},
        {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"},
    };
    BitgetFuturesKillSwitch duplicate_code(
        wrap(post), "USDT-FUTURES", "BTCUSDT");
    EXPECT_FALSE(duplicate_code.cancel_all_and_flatten(
        std::chrono::seconds(1)));

    post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"00000","msg":"success","data":{"list":[{"orderId":"7","clientOid":"tt-7","code":"25227","msg":"no position"}]}})"},
        {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"},
    };
    BitgetFuturesKillSwitch close_only_code_on_cancel(
        wrap(post), "USDT-FUTURES", "BTCUSDT");
    EXPECT_FALSE(close_only_code_on_cancel.cancel_all_and_flatten(
        std::chrono::seconds(1)));

    post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"},
        {200, R"({"code":"00000","msg":"success","data":{"list":[{"orderId":"8","clientOid":"tt-8","code":"25204","msg":"order missing"}]}})"},
    };
    BitgetFuturesKillSwitch cancel_only_code_on_close(
        wrap(post), "USDT-FUTURES", "BTCUSDT");
    EXPECT_FALSE(cancel_only_code_on_close.cancel_all_and_flatten(
        std::chrono::seconds(1)));
}

TEST(BitgetFuturesKillSwitch, HttpFailOnCloseReturnsFalse)
{
    SilenceStderr quiet;
    auto post = std::make_shared<fake_post>();
    post->responses = {
        {200, R"({"code":"00000","msg":"success","data":{"list":[]}})"},
        {503, R"({"code":"50000","msg":"unavailable"})"},
    };
    BitgetFuturesKillSwitch ks(
        wrap(post), "USDT-FUTURES", "BTCUSDT", position_readback());

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
    EXPECT_EQ(post->call_count(), 2u);
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

TEST(BitgetFuturesKillSwitch, PassesShrinkingAbsoluteDeadlineToCalls)
{
    auto post = std::make_shared<fake_post>();
    post->sleep_on_call = std::chrono::milliseconds(10);
    BitgetFuturesKillSwitch ks(
        wrap(post), "USDT-FUTURES", "BTCUSDT", position_readback());
    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::milliseconds(4500)));
    ASSERT_EQ(post->deadlines.size(), 2u);
    EXPECT_GT(post->deadlines[0].count(), 0);
    EXPECT_LT(post->deadlines[1], post->deadlines[0]);
}

TEST(BitgetFuturesKillSwitch, NoopCodeHelpers)
{
    EXPECT_TRUE(BitgetFuturesKillSwitch::is_cancel_noop_code("25204"));
    EXPECT_FALSE(BitgetFuturesKillSwitch::is_cancel_noop_code("22001"));
    EXPECT_FALSE(BitgetFuturesKillSwitch::is_cancel_noop_code("24056"));
    EXPECT_FALSE(BitgetFuturesKillSwitch::is_cancel_noop_code("00000"));
    EXPECT_TRUE(BitgetFuturesKillSwitch::is_close_noop_code("25227"));
    EXPECT_FALSE(BitgetFuturesKillSwitch::is_close_noop_code("22002"));
    EXPECT_FALSE(BitgetFuturesKillSwitch::is_close_noop_code("25601"));
    EXPECT_FALSE(BitgetFuturesKillSwitch::is_close_noop_code("40762"));
}

#endif // HAS_BITGET
