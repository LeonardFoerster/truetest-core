// Compile-only smoke tests for BinanceUserDataTransport. Full behavioural
// tests live in the integration suite against a mock exchange because this
// class owns real TLS WebSocket I/O and listenKey REST lifecycle.

#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_user_data_transport.h"

#include <memory>
#include <atomic>

TEST(BinanceUserDataTransport, ConstructDoesNotOpenConnections)
{
    BinanceUserDataTransport tx(nullptr);
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::closed);
    EXPECT_TRUE(tx.listen_key().empty());
}

TEST(BinanceUserDataTransport, OpenWithoutRestReportsError)
{
    BinanceUserDataTransport tx(nullptr);
    IFillTransport::lifecycle seen = IFillTransport::lifecycle::closed;
    tx.set_on_status([&](IFillTransport::lifecycle s, std::string_view) {
        seen = s;
    });
    EXPECT_FALSE(tx.open());
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::error);
    EXPECT_EQ(seen,       IFillTransport::lifecycle::error);
}

TEST(BinanceUserDataTransport, DestructorWithNoOpenIsClean)
{
    // just making sure the destructor doesn't deadlock when open() was never
    // successful - regressing this would hang the whole test binary.
    BinanceUserDataTransport tx(nullptr);
    (void)tx.open();
}

TEST(BinanceUserDataTransport, InitialHandshakeFailureRefusesReadyAndCleansKey)
{
    std::atomic<int> deletes{0};
    BinanceUserDataTransport tx(
        [] {
            BinanceRestClient::response r;
            r.status = 200;
            r.body = R"({"listenKey":"test-key"})";
            return r;
        },
        [&](const std::string& key) {
            EXPECT_EQ(key, "test-key");
            deletes.fetch_add(1, std::memory_order_relaxed);
        },
        [](std::atomic<bool>&) {
            return BinanceUserDataTransport::run_result::handshake_error;
        });

    bool saw_error = false;
    tx.set_on_status([&](IFillTransport::lifecycle s, std::string_view) {
        if (s == IFillTransport::lifecycle::error) saw_error = true;
    });

    EXPECT_FALSE(tx.open());
    EXPECT_TRUE(saw_error);
    EXPECT_EQ(deletes.load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(tx.listen_key().empty());
}

TEST(BinanceUserDataTransport, ListenKeyRequiresAuthoritativeUniqueEnvelope)
{
    const std::string bodies[] = {
        "{}",
        R"({"listenKey":"first","listenKey":"second"})",
        "{\"listenKey\":\"key\"} trailing",
        R"({"listenKey":null})",
        R"({"nested":{"listenKey":"key"}})",
    };
    for (const auto& body : bodies)
    {
        BinanceUserDataTransport tx(
            [body] {
                BinanceRestClient::response r;
                r.status = 200;
                r.body = body;
                return r;
            },
            [](const std::string&) {},
            [](std::atomic<bool>&) {
                return BinanceUserDataTransport::run_result::stopped;
            });
        EXPECT_FALSE(tx.open()) << body;
    }

    EXPECT_EQ(binance_keepalive_detail::authoritative_listen_key(
                  R"({"listenKey":"rotated"})"),
              "rotated");
    EXPECT_TRUE(binance_keepalive_detail::authoritative_listen_key(
                    R"({"listenKey":"a","listenKey":"b"})")
                    .empty());
    EXPECT_TRUE(binance_keepalive_detail::authoritative_listen_key(
                    "{\"listenKey\":\"a\"} trailing")
                    .empty());
}

TEST(BinanceUserDataTransport, TerminalKeepaliveWakesReaderAndPublishesFatal)
{
    for (const bool throw_from_keepalive : {false, true})
    {
        std::atomic<bool> reader_entered{false};
        std::atomic<bool> allow_keepalive{false};
        std::atomic<int> fatal_calls{0};
        std::atomic<int> deletes{0};
        binance_keepalive_policy policy;
        policy.interval = std::chrono::seconds{0};
        policy.retry_delay = std::chrono::seconds{0};
        policy.max_retries = 1;

        BinanceUserDataTransport tx(
            [] {
                BinanceRestClient::response r;
                r.status = 200;
                r.body = R"({"listenKey":"test-key"})";
                return r;
            },
            [&](const std::string&) {
                deletes.fetch_add(1, std::memory_order_release);
            },
            [&](std::atomic<bool>& stop) {
                reader_entered.store(true, std::memory_order_release);
                reader_entered.notify_all();
                stop.wait(false, std::memory_order_acquire);
                return BinanceUserDataTransport::run_result::stopped;
            },
            [&, throw_from_keepalive]()
                -> binance_keepalive_detail::tick_result {
                allow_keepalive.wait(false, std::memory_order_acquire);
                if (throw_from_keepalive) throw 7;
                binance_keepalive_detail::tick_result r;
                r.k = binance_keepalive_detail::tick_result::kind::error;
                r.note = "forced keepalive failure";
                return r;
            },
            policy,
            true);
        tx.set_fatal_disconnect_callback([&](std::string_view) {
            fatal_calls.fetch_add(1, std::memory_order_release);
            fatal_calls.notify_all();
        });

        ASSERT_TRUE(tx.open());
        reader_entered.wait(false, std::memory_order_acquire);
        allow_keepalive.store(true, std::memory_order_release);
        allow_keepalive.notify_all();
        int expected = 0;
        while (fatal_calls.load(std::memory_order_acquire) == 0)
            fatal_calls.wait(expected, std::memory_order_acquire);
        const auto start = std::chrono::steady_clock::now();
        tx.close();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        EXPECT_LT(elapsed, std::chrono::milliseconds(500));
        EXPECT_EQ(fatal_calls.load(std::memory_order_acquire), 1);
        EXPECT_EQ(deletes.load(std::memory_order_acquire), 1);
        EXPECT_EQ(tx.state(), IFillTransport::lifecycle::closed);
    }
}

#endif // HAS_BINANCE
