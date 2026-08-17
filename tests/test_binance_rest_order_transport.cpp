#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_rest_order_transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {

struct fake_caller
{
    int status = 200;
    std::string body;
    std::string last_endpoint;
    std::string last_params;
    int calls = 0;

    BinanceRestOrderTransport::response operator()(std::string_view ep,
                                                   std::string_view params)
    {
        last_endpoint = std::string(ep);
        last_params   = std::string(params);
        ++calls;
        return {status, body};
    }
};

}

TEST(BinanceRestOrderTransport, SubmitSuccessExtractsExchangeOrderId)
{
    auto post = std::make_shared<fake_caller>();
    post->status = 200;
    post->body   = R"({"orderId":999123,"status":"NEW"})";
    auto del     = std::make_shared<fake_caller>();

    BinanceRestOrderTransport tx(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); });

    auto r = tx.submit("/api/v3/order", "symbol=BTCUSDT&side=BUY");

    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.exchange_order_id, "999123");
    EXPECT_EQ(r.raw_response, post->body);
    EXPECT_EQ(post->last_endpoint, "/api/v3/order");
    EXPECT_EQ(post->last_params, "symbol=BTCUSDT&side=BUY");
    EXPECT_EQ(del->calls, 0);
}

TEST(BinanceRestOrderTransport, SubmitSuccessHandlesStringOrderId)
{
    auto post = std::make_shared<fake_caller>();
    post->status = 201;
    post->body   = R"({"orderId":"999124"})";

    BinanceRestOrderTransport tx(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        {});

    auto r = tx.submit("/api/v3/order", "");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.exchange_order_id, "999124");
}

TEST(BinanceRestOrderTransport, SubmitNon2xxReturnsError)
{
    auto post = std::make_shared<fake_caller>();
    post->status = 400;
    post->body   = R"({"code":-1021,"msg":"Timestamp for this request was 1000ms ahead."})";

    BinanceRestOrderTransport tx(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        {});

    auto r = tx.submit("/api/v3/order", "bad=1");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.exchange_order_id.empty());
    EXPECT_NE(r.error.find("HTTP 400"), std::string::npos);
    EXPECT_NE(r.error.find("Timestamp"), std::string::npos);
}

TEST(BinanceRestOrderTransport, PostWriteServerErrorIsTerminallyUncertain)
{
    BinanceRestOrderTransport tx(
        [](std::string_view, std::string_view) {
            return BinanceRestOrderTransport::response{
                503, R"({"code":-1000,"msg":"Unknown error"})", true, false};
        }, {});
    const auto r = tx.submit("/api/v3/order", "symbol=BTCUSDT");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.uncertain);
    EXPECT_TRUE(r.fatal);
    EXPECT_NE(r.error.find("ambiguous"), std::string::npos);
}

TEST(BinanceRestOrderTransport, CancelRoutesToDelCallable)
{
    auto post = std::make_shared<fake_caller>();
    auto del  = std::make_shared<fake_caller>();
    del->status = 200;
    del->body   = R"({"orderId":999,"status":"CANCELED"})";

    BinanceRestOrderTransport tx(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del ](std::string_view ep, std::string_view p) { return (*del )(ep, p); });

    auto r = tx.cancel("/api/v3/order", "symbol=BTCUSDT&orderId=999");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(del->last_params, "symbol=BTCUSDT&orderId=999");
    EXPECT_EQ(post->calls, 0);
}

TEST(BinanceRestOrderTransport, MalformedSuccessIsTerminallyUncertain)
{
    auto post = std::make_shared<fake_caller>();
    auto del = std::make_shared<fake_caller>();
    BinanceRestOrderTransport tx(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del](std::string_view ep, std::string_view p) { return (*del)(ep, p); });

    const std::string submit_payloads[] = {
        "{}",
        "<html>ok</html>",
        R"({"orderId":1,"orderId":2})",
        "{\"orderId\":1} trailing",
        R"({"orderId":1,"clientOrderId":"wrong"})",
        R"({"nested":{"orderId":1,"clientOrderId":"tt-7"}})",
    };
    for (const auto& body : submit_payloads)
    {
        post->body = body;
        const auto r = tx.submit(
            "/api/v3/order", "newClientOrderId=tt-7&symbol=BTCUSDT");
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(r.uncertain) << body;
        EXPECT_TRUE(r.fatal) << body;
    }

    const std::string cancel_payloads[] = {
        R"({"status":"CANCELED"})",
        R"({"orderId":999,"status":"NEW"})",
        R"({"orderId":998,"status":"CANCELED"})",
        R"({"orderId":999,"orderId":998,"status":"CANCELED"})",
        R"({"nested":{"orderId":999,"status":"CANCELED"}})",
    };
    for (const auto& body : cancel_payloads)
    {
        del->body = body;
        const auto r = tx.cancel(
            "/api/v3/order", "symbol=BTCUSDT&orderId=999");
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(r.uncertain) << body;
        EXPECT_TRUE(r.fatal) << body;
    }
}

TEST(BinanceRestOrderTransport, SubmitNullCallableReturnsError)
{
    BinanceRestOrderTransport tx({}, {});
    auto r = tx.submit("/api/v3/order", "x=1");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST(BinanceRestOrderTransport, CancelNullCallableReturnsError)
{
    BinanceRestOrderTransport tx({}, {});
    auto r = tx.cancel("/api/v3/order", "x=1");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST(BinanceRestOrderTransport, OpenCloseAreNoOps)
{
    BinanceRestOrderTransport tx({}, {});
    EXPECT_TRUE(tx.open());
    tx.close();
}

TEST(BinanceRestOrderTransport, QuiesceRejectsLateSubmitAndCancelWithoutCallingVenue)
{
    auto post = std::make_shared<fake_caller>();
    auto del = std::make_shared<fake_caller>();
    BinanceRestOrderTransport tx(
        [post](std::string_view ep, std::string_view p) { return (*post)(ep, p); },
        [del](std::string_view ep, std::string_view p) { return (*del)(ep, p); });

    tx.quiesce();
    const auto submit = tx.submit("/order", "x=1");
    const auto cancel = tx.cancel("/order", "x=1");

    EXPECT_FALSE(submit.ok);
    EXPECT_FALSE(cancel.ok);
    EXPECT_EQ(post->calls, 0);
    EXPECT_EQ(del->calls, 0);
}

TEST(BinanceRestOrderTransport, QuiesceWaitsForAdmittedMutationAndBlocksLaterCalls)
{
    std::mutex mu;
    std::condition_variable entered_cv;
    std::condition_variable release_cv;
    bool entered = false;
    bool release = false;
    std::atomic<int> calls{0};
    std::atomic<bool> mutation_completed{false};
    BinanceRestOrderTransport tx(
        [&](std::string_view, std::string_view) {
            {
                std::lock_guard<std::mutex> lock(mu);
                entered = true;
            }
            entered_cv.notify_all();
            std::unique_lock<std::mutex> lock(mu);
            release_cv.wait(lock, [&] { return release; });
            calls.fetch_add(1, std::memory_order_release);
            mutation_completed.store(true, std::memory_order_release);
            return BinanceRestOrderTransport::response{200, R"({"orderId":"1"})"};
        }, {});

    std::thread submitter([&] { (void)tx.submit("/order", "x=1"); });
    {
        std::unique_lock<std::mutex> lock(mu);
        entered_cv.wait(lock, [&] { return entered; });
    }
    std::atomic<bool> quiesced{false};
    std::atomic<bool> quiesce_saw_completion{false};
    std::thread stopper([&] {
        tx.quiesce();
        quiesce_saw_completion.store(
            mutation_completed.load(std::memory_order_acquire),
            std::memory_order_release);
        quiesced.store(true, std::memory_order_release);
    });
    {
        std::lock_guard<std::mutex> lock(mu);
        release = true;
    }
    release_cv.notify_all();
    submitter.join();
    stopper.join();

    EXPECT_TRUE(quiesced.load(std::memory_order_acquire));
    EXPECT_TRUE(quiesce_saw_completion.load(std::memory_order_acquire));
    EXPECT_EQ(calls.load(std::memory_order_acquire), 1);
    EXPECT_FALSE(tx.submit("/order", "x=2").ok);
    EXPECT_EQ(calls.load(std::memory_order_acquire), 1);
}

#endif // HAS_BINANCE
