#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_futures_order_encoder.h"
#include "providers/bitget/bitget_rest_order_transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {

static auto now() { return std::chrono::system_clock::now(); }

order_event make_order(const std::string& symbol,
                       order_type type,
                       order_side side,
                       double qty,
                       double price = 0.0,
                       time_in_force tif = time_in_force::gtc)
{
    order_event o(now(), symbol, type, side, qty, price, tif);
    o.set_order_id(1);
    return o;
}

struct fake_post
{
    int status = 200;
    std::string body;
    std::string last_endpoint;
    std::string last_body;
    int calls = 0;

    BitgetRestOrderTransport::response operator()(std::string_view ep,
                                                  std::string_view b)
    {
        last_endpoint = std::string(ep);
        last_body     = std::string(b);
        ++calls;
        return {status, body};
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Encoder — place LIMIT / MARKET / cancel exact JSON
// ---------------------------------------------------------------------------

TEST(BitgetFuturesOrderEncoder, SubmitLimitExactJson)
{
    BitgetFuturesOrderEncoder enc;
    auto o = make_order("btcusdt", order_type::limit, order_side::buy,
                        0.001, 90000.0, time_in_force::gtc);

    auto e = enc.encode_submit(o, "tt-1");

    EXPECT_EQ(e.endpoint, "/api/v3/trade/place-order");
    EXPECT_EQ(e.client_order_id, "tt-1");
    EXPECT_EQ(e.wire_payload,
              R"({"category":"USDT-FUTURES","symbol":"BTCUSDT","side":"buy","orderType":"limit","qty":"0.001","price":"90000","timeInForce":"gtc","clientOid":"tt-1","reduceOnly":"no","marginMode":"crossed"})");
}

TEST(BitgetFuturesOrderEncoder, SubmitMarketExactJson)
{
    BitgetFuturesOrderEncoder enc;
    auto o = make_order("btcusdt", order_type::market, order_side::sell,
                        0.001);

    auto e = enc.encode_submit(o, "tt-m");

    EXPECT_EQ(e.endpoint, "/api/v3/trade/place-order");
    EXPECT_EQ(e.wire_payload,
              R"({"category":"USDT-FUTURES","symbol":"BTCUSDT","side":"sell","orderType":"market","qty":"0.001","clientOid":"tt-m","reduceOnly":"no"})");
    // Market must not carry price / TIF / marginMode
    EXPECT_EQ(e.wire_payload.find("price"), std::string::npos);
    EXPECT_EQ(e.wire_payload.find("timeInForce"), std::string::npos);
    EXPECT_EQ(e.wire_payload.find("marginMode"), std::string::npos);
}

TEST(BitgetFuturesOrderEncoder, CancelByOrderIdExactJson)
{
    BitgetFuturesOrderEncoder enc;
    auto e = enc.encode_cancel("BTCUSDT", "121211212122", "tt-1");

    EXPECT_EQ(e.endpoint, "/api/v3/trade/cancel-order");
    EXPECT_EQ(e.wire_payload,
              R"({"category":"USDT-FUTURES","orderId":"121211212122"})");
    // Prefer exchange id — clientOid must not appear
    EXPECT_EQ(e.wire_payload.find("clientOid"), std::string::npos);
}

TEST(BitgetFuturesOrderEncoder, CancelByClientOidExactJson)
{
    BitgetFuturesOrderEncoder enc;
    auto e = enc.encode_cancel("BTCUSDT", "", "tt-cli-1");

    EXPECT_EQ(e.endpoint, "/api/v3/trade/cancel-order");
    EXPECT_EQ(e.wire_payload,
              R"({"category":"USDT-FUTURES","clientOid":"tt-cli-1"})");
}

TEST(BitgetFuturesOrderEncoder, StopTypesRefuseEncode)
{
    BitgetFuturesOrderEncoder enc;
    auto stop = make_order("BTCUSDT", order_type::stop, order_side::sell,
                           1.0, 0.0);
    auto sl   = make_order("BTCUSDT", order_type::stop_limit, order_side::buy,
                           1.0, 51000.0);

    auto e1 = enc.encode_submit(stop, "tt-s");
    auto e2 = enc.encode_submit(sl, "tt-sl");

    EXPECT_TRUE(e1.endpoint.empty());
    EXPECT_TRUE(e1.wire_payload.empty());
    EXPECT_TRUE(e2.endpoint.empty());
    EXPECT_TRUE(e2.wire_payload.empty());
}

TEST(BitgetFuturesOrderEncoder, ReduceOnlyYes)
{
    BitgetFuturesOrderEncoder enc;
    enc.set_reduce_only(true);
    auto o = make_order("BTCUSDT", order_type::market, order_side::sell, 0.01);
    auto e = enc.encode_submit(o, "tt-ro");
    EXPECT_NE(e.wire_payload.find(R"("reduceOnly":"yes")"), std::string::npos);
}

TEST(BitgetFuturesOrderEncoder, TifMappingLowercase)
{
    BitgetFuturesOrderEncoder enc;
    auto ioc = enc.encode_submit(
        make_order("A", order_type::limit, order_side::buy, 1, 1, time_in_force::ioc),
        "c1");
    auto fok = enc.encode_submit(
        make_order("A", order_type::limit, order_side::buy, 1, 1, time_in_force::fok),
        "c2");
    auto day = enc.encode_submit(
        make_order("A", order_type::limit, order_side::buy, 1, 1, time_in_force::day),
        "c3");

    EXPECT_NE(ioc.wire_payload.find(R"("timeInForce":"ioc")"), std::string::npos);
    EXPECT_NE(fok.wire_payload.find(R"("timeInForce":"fok")"), std::string::npos);
    EXPECT_NE(day.wire_payload.find(R"("timeInForce":"gtc")"), std::string::npos);
}

TEST(BitgetFuturesOrderEncoder, DefaultSymbolWhenEmpty)
{
    BitgetFuturesOrderEncoder enc("ethusdt");
    auto o = make_order("", order_type::limit, order_side::buy, 1.0, 20.0);
    auto e = enc.encode_submit(o, "tt-7");
    EXPECT_NE(e.wire_payload.find(R"("symbol":"ETHUSDT")"), std::string::npos);
}

TEST(BitgetFuturesOrderEncoder, InvalidClientOidRefuses)
{
    BitgetFuturesOrderEncoder enc;
    auto o = make_order("BTCUSDT", order_type::limit, order_side::buy, 1, 1);
    // space is illegal
    auto e = enc.encode_submit(o, "bad id");
    EXPECT_TRUE(e.endpoint.empty());
    EXPECT_TRUE(e.wire_payload.empty());

    EXPECT_FALSE(BitgetFuturesOrderEncoder::valid_client_oid(""));
    EXPECT_FALSE(BitgetFuturesOrderEncoder::valid_client_oid(
        std::string(33, 'a')));
    EXPECT_TRUE(BitgetFuturesOrderEncoder::valid_client_oid("tt-1.A:z/0_"));
}

// ---------------------------------------------------------------------------
// Transport — POST cancel, business code gate, inject post_fn
// ---------------------------------------------------------------------------

TEST(BitgetRestOrderTransport, SubmitSuccessExtractsOrderIdFromData)
{
    auto post = std::make_shared<fake_post>();
    post->status = 200;
    post->body =
        R"({"code":"00000","msg":"success","data":{"orderId":"999123","clientOid":"tt-1"}})";

    BitgetRestOrderTransport tx(
        [post](std::string_view ep, std::string_view b) { return (*post)(ep, b); });

    auto r = tx.submit("/api/v3/trade/place-order",
                       R"({"category":"USDT-FUTURES","symbol":"BTCUSDT"})");

    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.exchange_order_id, "999123");
    EXPECT_EQ(r.raw_response, post->body);
    EXPECT_EQ(post->last_endpoint, "/api/v3/trade/place-order");
    EXPECT_EQ(post->calls, 1);
}

TEST(BitgetRestOrderTransport, CancelUsesPostNotDelete)
{
    auto post = std::make_shared<fake_post>();
    post->status = 200;
    post->body =
        R"({"code":"00000","msg":"success","data":{"orderId":"42"}})";

    BitgetRestOrderTransport tx(
        [post](std::string_view ep, std::string_view b) { return (*post)(ep, b); });

    auto r = tx.cancel("/api/v3/trade/cancel-order",
                       R"({"category":"USDT-FUTURES","orderId":"42"})");

    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.exchange_order_id, "42");
    EXPECT_EQ(post->last_endpoint, "/api/v3/trade/cancel-order");
    EXPECT_EQ(post->last_body,
              R"({"category":"USDT-FUTURES","orderId":"42"})");
    EXPECT_EQ(post->calls, 1); // single post path — no separate del
}

TEST(BitgetRestOrderTransport, MalformedSuccessIsTerminallyUncertain)
{
    auto post = std::make_shared<fake_post>();
    BitgetRestOrderTransport tx(
        [post](std::string_view ep, std::string_view b) { return (*post)(ep, b); });

    const std::string submit_payloads[] = {
        R"({"code":"00000"})",
        R"({"code":"00000","data":{}})",
        R"({"code":"00000","data":{"orderId":"1","orderId":"2","clientOid":"tt-1"}})",
        R"({"code":"00000","orderId":"1","data":{}})",
        R"({"code":"00000","data":{"orderId":"1","clientOid":"wrong"}})",
        R"({"nested":{"code":"00000","data":{"orderId":"1","clientOid":"tt-1"}}})",
        R"({"code":"00000","data":{"nested":{"orderId":"1","clientOid":"tt-1"}}})",
        "{\"code\":\"00000\",\"data\":{\"orderId\":\"1\",\"clientOid\":\"tt-1\"}} trailing",
    };
    for (const auto& body : submit_payloads)
    {
        post->body = body;
        const auto r = tx.submit(
            "/api/v3/trade/place-order", "{\"clientOid\":\"tt-1\"}");
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(r.uncertain) << body;
        EXPECT_TRUE(r.fatal) << body;
    }

    const std::string cancel_payloads[] = {
        R"({"code":"00000","data":{}})",
        R"({"code":"00000","data":{"orderId":"41"}})",
        R"({"code":"00000","data":{"orderId":"42","orderId":"43"}})",
        R"({"code":"00000","data":{"nested":{"orderId":"42"}}})",
    };
    for (const auto& body : cancel_payloads)
    {
        post->body = body;
        const auto r = tx.cancel(
            "/api/v3/trade/cancel-order", "{\"orderId\":\"42\"}");
        EXPECT_FALSE(r.ok) << body;
        EXPECT_TRUE(r.uncertain) << body;
        EXPECT_TRUE(r.fatal) << body;
    }
}

TEST(BitgetRestOrderTransport, BusinessCodeNot00000Fails)
{
    auto post = std::make_shared<fake_post>();
    post->status = 200; // HTTP ok, business fail — critical Bitget quirk
    post->body =
        R"({"code":"43011","msg":"Insufficient balance","requestTime":1,"data":null})";

    BitgetRestOrderTransport tx(
        [post](std::string_view ep, std::string_view b) { return (*post)(ep, b); });

    auto r = tx.submit("/api/v3/trade/place-order", "{}");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.exchange_order_id.empty());
    EXPECT_NE(r.error.find("43011"), std::string::npos);
}

TEST(BitgetRestOrderTransport, HttpNon2xxFails)
{
    auto post = std::make_shared<fake_post>();
    post->status = 400;
    post->body   = R"({"code":"40001","msg":"bad"})";

    BitgetRestOrderTransport tx(
        [post](std::string_view ep, std::string_view b) { return (*post)(ep, b); });

    auto r = tx.submit("/api/v3/trade/place-order", "{}");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("HTTP 400"), std::string::npos);
}

TEST(BitgetRestOrderTransport, PostWriteServerErrorIsTerminallyUncertain)
{
    BitgetRestOrderTransport tx(
        [](std::string_view, std::string_view) {
            return BitgetRestOrderTransport::response{
                503, R"({"code":"50000","msg":"unknown"})", true, false};
        });
    const auto r = tx.submit("/api/v3/trade/place-order", "{}");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.uncertain);
    EXPECT_TRUE(r.fatal);
    EXPECT_NE(r.error.find("ambiguous"), std::string::npos);
}

TEST(BitgetRestOrderTransport, NullPostCallableReturnsError)
{
    BitgetRestOrderTransport tx({});
    auto s = tx.submit("/api/v3/trade/place-order", "{}");
    auto c = tx.cancel("/api/v3/trade/cancel-order", "{}");
    EXPECT_FALSE(s.ok);
    EXPECT_FALSE(c.ok);
    EXPECT_FALSE(s.error.empty());
    EXPECT_FALSE(c.error.empty());
}

TEST(BitgetRestOrderTransport, OpenCloseAreNoOps)
{
    BitgetRestOrderTransport tx({});
    EXPECT_TRUE(tx.open());
    tx.close();
}

TEST(BitgetRestOrderTransport, NumericOrderIdExtracted)
{
    auto post = std::make_shared<fake_post>();
    post->status = 200;
    post->body =
        R"({"code":"00000","msg":"success","data":{"orderId":121211212122}})";

    BitgetRestOrderTransport tx(
        [post](std::string_view ep, std::string_view b) { return (*post)(ep, b); });

    auto r = tx.submit("/api/v3/trade/place-order", "{}");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.exchange_order_id, "121211212122");
}

TEST(BitgetRestOrderTransport, QuiesceRejectsLateSubmitAndCancelWithoutCallingVenue)
{
    auto post = std::make_shared<fake_post>();
    BitgetRestOrderTransport tx(
        [post](std::string_view ep, std::string_view b) { return (*post)(ep, b); });

    tx.quiesce();
    const auto submit = tx.submit("/api/v3/trade/place-order", "{}");
    const auto cancel = tx.cancel("/api/v3/trade/cancel-order", "{}");

    EXPECT_FALSE(submit.ok);
    EXPECT_FALSE(cancel.ok);
    EXPECT_EQ(post->calls, 0);
}

TEST(BitgetRestOrderTransport, QuiesceWaitsForAdmittedMutationAndBlocksLaterCalls)
{
    std::mutex mu;
    std::condition_variable entered_cv;
    std::condition_variable release_cv;
    bool entered = false;
    bool release = false;
    std::atomic<int> calls{0};
    std::atomic<bool> mutation_completed{false};
    BitgetRestOrderTransport tx(
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
            return BitgetRestOrderTransport::response{
                200, R"({"code":"00000","data":{"orderId":"1"}})"};
        });

    std::thread submitter([&] { (void)tx.submit("/order", "{}"); });
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
    EXPECT_FALSE(tx.submit("/order", "{}").ok);
    EXPECT_EQ(calls.load(std::memory_order_acquire), 1);
}

#endif // HAS_BITGET
